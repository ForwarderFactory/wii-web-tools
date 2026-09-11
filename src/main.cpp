#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <netkit/http/async_server.hpp>
#include <netkit/body/async_file_body.hpp>
#include <netkit/body/async_buffer_body.hpp>
#include <netkit/http/multipart.hpp>
#include <netkit/http/async_multipart_reader.hpp>
#include <nlohmann/json.hpp>

constexpr int PORT = 8080;
const std::string TEMP_DIRECTORY = "/tmp/wii-banner-renderer";
const std::string RENDERS_INDEX_FILE = TEMP_DIRECTORY + "/renders_index.json";
constexpr std::size_t MAX_FILES_PER_REQUEST = 5;
constexpr std::size_t MAX_REQUEST_SIZE = 128000000;
constexpr auto RENDER_TTL = std::chrono::hours(24 * 14); // 2 weeks
constexpr auto CLEANUP_INTERVAL = std::chrono::hours(1);
constexpr std::size_t MAX_RECENT_RENDERS = 24;

namespace sha256_impl {
    struct sha256_ctx {
        std::uint32_t state[8] = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
        };
        std::uint64_t bit_len = 0;
        std::uint8_t buffer[64]{};
        std::size_t buffer_len = 0;
    };

    inline std::uint32_t rotr(std::uint32_t x, std::uint32_t n) { return (x >> n) | (x << (32 - n)); }

    inline void transform(sha256_ctx& ctx, const std::uint8_t* data) {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
        };

        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(data[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(data[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(data[i * 4 + 2]) << 8) |
                   (static_cast<std::uint32_t>(data[i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = ctx.state[0], b = ctx.state[1], c = ctx.state[2], d = ctx.state[3];
        std::uint32_t e = ctx.state[4], f = ctx.state[5], g = ctx.state[6], h = ctx.state[7];

        for (int i = 0; i < 64; ++i) {
            std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            std::uint32_t ch = (e & f) ^ (~e & g);
            std::uint32_t temp1 = h + s1 + ch + k[i] + w[i];
            std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t temp2 = s0 + maj;

            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        ctx.state[0] += a; ctx.state[1] += b; ctx.state[2] += c; ctx.state[3] += d;
        ctx.state[4] += e; ctx.state[5] += f; ctx.state[6] += g; ctx.state[7] += h;
    }

    inline void update(sha256_ctx& ctx, const std::uint8_t* data, std::size_t len) {
        ctx.bit_len += static_cast<std::uint64_t>(len) * 8;

        while (len > 0) {
            std::size_t to_copy = std::min(len, sizeof(ctx.buffer) - ctx.buffer_len);
            std::memcpy(ctx.buffer + ctx.buffer_len, data, to_copy);
            ctx.buffer_len += to_copy;
            data += to_copy;
            len -= to_copy;

            if (ctx.buffer_len == sizeof(ctx.buffer)) {
                transform(ctx, ctx.buffer);
                ctx.buffer_len = 0;
            }
        }
    }

    inline std::string finalize(sha256_ctx& ctx) {
        std::uint64_t bit_len = ctx.bit_len;

        std::uint8_t pad = 0x80;
        update(ctx, &pad, 1);

        std::uint8_t zero = 0x00;
        while (ctx.buffer_len != 56) {
            update(ctx, &zero, 1);
        }

        std::uint8_t len_bytes[8];
        for (int i = 0; i < 8; ++i) {
            len_bytes[i] = static_cast<std::uint8_t>(bit_len >> (56 - i * 8));
        }
        // append length directly to avoid recursing through update()'s padding logic
        std::memcpy(ctx.buffer + ctx.buffer_len, len_bytes, 8);
        transform(ctx, ctx.buffer);

        static constexpr char hex_chars[] = "0123456789abcdef";
        std::string result;
        result.reserve(64);
        for (std::uint32_t s : ctx.state) {
            for (int shift = 24; shift >= 0; shift -= 8) {
                std::uint8_t byte = static_cast<std::uint8_t>(s >> shift);
                result += hex_chars[byte >> 4];
                result += hex_chars[byte & 0x0F];
            }
        }
        return result;
    }
}

class streaming_hasher {
public:
    void update(const char* data, std::size_t len) {
        sha256_impl::update(ctx_, reinterpret_cast<const std::uint8_t*>(data), len);
    }

    std::string hex_digest() {
        return sha256_impl::finalize(ctx_);
    }

private:
    sha256_impl::sha256_ctx ctx_{};
};

enum class status {
    processing,
    finished
};

struct banner_tracker {
    std::string key{};
    std::atomic<status> render_status;
    std::string actual_filename{};
    std::string original_filename{};
    std::string content_hash{};
    bool icon{false};
    bool hidden{false};
    std::int64_t uploaded_at{0};

    banner_tracker& operator=(const banner_tracker& other) {
        key = other.key;
        actual_filename = other.actual_filename;
        original_filename = other.original_filename;
        content_hash = other.content_hash;
        icon = other.icon;
        hidden = other.hidden;
        uploaded_at = other.uploaded_at;
        render_status.store(other.render_status.load());
        return *this;
    }
};

std::unordered_map<std::string, banner_tracker> banner_trackers;
std::mutex banner_trackers_mutex;

std::int64_t now_unix_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

void save_renders_index_locked() {
    nlohmann::json out = nlohmann::json::object();

    for (const auto& [key, tracker] : banner_trackers) {
        out[key] = {
            {"actual_filename", tracker.actual_filename},
            {"original_filename", tracker.original_filename},
            {"content_hash", tracker.content_hash},
            {"icon", tracker.icon},
            {"hidden", tracker.hidden},
            {"uploaded_at", tracker.uploaded_at},
            {"finished", tracker.render_status.load() == status::finished},
        };
    }

    std::ofstream of{RENDERS_INDEX_FILE, std::ofstream::trunc};
    of << out.dump();
}

void load_renders_index() {
    if (!std::filesystem::is_regular_file(RENDERS_INDEX_FILE)) {
        return;
    }

    nlohmann::json in;
    try {
        std::ifstream ifs{RENDERS_INDEX_FILE};
        in = nlohmann::json::parse(ifs);
    } catch (std::exception& e) {
        std::cerr << "Failed to load renders index: " << e.what() << "\n";
        return;
    }

    std::lock_guard<std::mutex> lock(banner_trackers_mutex);

    for (auto it = in.begin(); it != in.end(); ++it) {
        const std::string& key = it.key();
        const auto& v = it.value();

        std::string actual_filename = v.value("actual_filename", "");
        bool finished = v.value("finished", false);

        if (!finished || !std::filesystem::is_regular_file(actual_filename)) {
            continue;
        }

        banner_tracker tracker;
        tracker.key = key;
        tracker.actual_filename = actual_filename;
        tracker.original_filename = v.value("original_filename", "");
        tracker.content_hash = v.value("content_hash", "");
        tracker.icon = v.value("icon", false);
        tracker.hidden = v.value("hidden", false);
        tracker.uploaded_at = v.value("uploaded_at", static_cast<std::int64_t>(0));
        tracker.render_status = status::finished;

        banner_trackers[key] = tracker;
    }

    std::cout << "Loaded " << banner_trackers.size() << " render(s) from index\n";
}

[[noreturn]] void cleanup_old_renders() {
    while (true) {
        std::this_thread::sleep_for(CLEANUP_INTERVAL);

        auto cutoff = now_unix_seconds() - std::chrono::duration_cast<std::chrono::seconds>(RENDER_TTL).count();
        std::vector<std::string> expired_keys;

        {
            std::lock_guard<std::mutex> lock(banner_trackers_mutex);

            for (const auto& [key, tracker] : banner_trackers) {
                if (tracker.uploaded_at != 0 && tracker.uploaded_at < cutoff) {
                    expired_keys.push_back(key);
                }
            }

            for (const auto& key : expired_keys) {
                banner_trackers.erase(key);
            }

            if (!expired_keys.empty()) {
                save_renders_index_locked();
            }
        }

        for (const auto& key : expired_keys) {
            std::error_code ec;
            std::filesystem::remove_all(std::filesystem::path(TEMP_DIRECTORY) / key, ec);
            if (ec) {
                std::cerr << "Failed to remove expired render " << key << ": " << ec.message() << "\n";
            }
        }

        if (!expired_keys.empty()) {
            std::cout << "Cleanup: removed " << expired_keys.size() << " render(s) older than 2 weeks\n";
        }
    }
}

static constexpr char default_charset[] =
    "0123456789"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz";

std::string generate_random_string(const int length, const char* charset = default_charset) {
    static constexpr size_t charset_size = sizeof(charset) - 1;
    static std::random_device rd;
    static std::mt19937 generator(rd());

    std::uniform_int_distribution<> distribution(0, charset_size - 1);

    std::string str(length, 0);

    std::generate_n(str.begin(), length, [&distribution, &charset]() { return charset[distribution(generator)]; });

    return str;
}

netkit::io::task<netkit::http::server::async_response>
get_index(const netkit::http::server::async_request& req) {
    netkit::http::server::async_response resp;
    resp.content_type = "text/html";
    resp.http_status = 200;

    auto string = R"(
        <!DOCTYPE html>
        <html>
        <head>
            <title>Wii Web Tools</title>
            <meta charset="UTF-8">
            <link rel="stylesheet" href="/banner-renderer/style.css">
        </head>
        <body>
            <div class="container">
                <h1>Wii Web Tools by Forwarder Factory</h1>
                <p>This website provides some useful Wii tools right in your web browser.</p>
                <ul>
                    <li><a href="/banner-renderer/">Banner Renderer</a> - Render your Wii channels to a video file</li>
                    <li>More tools coming later.</li>
                </ul>
            </div>
        </body>
        </html>
        )";

    resp.body = netkit::body::make_body<netkit::body::async_buffer_body>(string);

    co_return resp;
}

netkit::io::task<netkit::http::server::async_response>
get_banner_renderer_index(const netkit::http::server::async_request& req) {
    netkit::http::server::async_response resp;
    resp.content_type = "text/html";
    resp.http_status = 200;

    auto string = R"(
        <!DOCTYPE html>
        <html>
        <head>
            <title>Wii Banner Renderer</title>
            <meta charset="UTF-8">
            <link rel="stylesheet" href="/banner-renderer/style.css">
        </head>
        <body>
            <div class="container">
                <h1>Wii Banner Renderer</h1>
                <p>Render your Wii channels into playable video files in your browser.</p>
                <p>You can select up to 5 .wad files at once.</p>
                <form id="upload_form" method="POST" enctype="multipart/form-data">
                    <input type="file" id="wad_input" name="wad" accept=".wad" multiple required>

                    <div id="file_list"></div>

                    <button type="submit" id="submit_button" disabled>Render</button>
                </form>
                <div id="renders"></div>
                <h2>Recently rendered</h2>
                <div class="carousel">
                    <button type="button" class="carousel_btn carousel_left" aria-label="Scroll left">&#8249;</button>
                    <div id="recently_rendered" class="carousel_track"></div>
                    <button type="button" class="carousel_btn carousel_right" aria-label="Scroll right">&#8250;</button>
                </div>
                <h2>Having issues?</h2>
                <p>Wii Banner Renderer is open source software, based on the work of the Wii Banner Player Project.</p>
                <p>Report any issues with rendering <a href="https://github.com/ForwarderFactory/wii-banner-renderer">here</a> using the 'broken forwarder' label.</p>
                <p>wii-web-tools (front- and backend, in other words this site) is also open source, see repository <a href="https://github.com/ForwarderFactory/wii-web-tools">here</a>.</p>
                <h2>Credits</h2>
                <ul>
                    <li>Wii Banner Player Project - original authors of most of the rendering logic</li>
                    <li>giantpune - improvements to the rendering</li>
                    <li>Dimok - improvements to the rendering</li>
                    <li>Jacob Nilsson - fork maintainer, rewrite, this site, server hosting</li>
                    <li>GitHub contributors - various things, usually things I can't be asked to do myself, very much appreciated</li>
                </ul>
            </div>
            <script>
                const MAX_FILES = 5;

                const form = document.getElementById('upload_form');
                const wad_input = document.getElementById('wad_input');
                const file_list = document.getElementById('file_list');
                const submit_button = document.getElementById('submit_button');
                const renders = document.getElementById('renders');

                let selected_files = [];

                function build_file_list() {
                    file_list.innerHTML = '';
                    selected_files = [];

                    for (const file of wad_input.files) {
                        const row = document.createElement('div');
                        row.className = 'file_row';

                        const name = document.createElement('span');
                        name.className = 'file_name';
                        name.textContent = file.name;

                        const label = document.createElement('label');
                        const icon_checkbox = document.createElement('input');
                        icon_checkbox.type = 'checkbox';
                        label.appendChild(icon_checkbox);
                        label.appendChild(document.createTextNode(' Icon'));

                        const both_label = document.createElement('label');
                        const both_checkbox = document.createElement('input');
                        both_checkbox.type = 'checkbox';
                        both_label.appendChild(both_checkbox);
                        both_label.appendChild(document.createTextNode(' Render both icon & banner'));

                        both_checkbox.addEventListener('change', () => {
                            icon_checkbox.disabled = both_checkbox.checked;
                        });

                        const hidden_label = document.createElement('label');
                        const hidden_checkbox = document.createElement('input');
                        hidden_checkbox.type = 'checkbox';
                        hidden_label.appendChild(hidden_checkbox);
                        hidden_label.appendChild(document.createTextNode(' Hide from recently rendered'));

                        row.appendChild(name);
                        row.appendChild(label);
                        row.appendChild(both_label);
                        row.appendChild(hidden_label);
                        file_list.appendChild(row);

                        selected_files.push({ file, icon_checkbox, both_checkbox, hidden_checkbox });
                    }

                    submit_button.disabled = selected_files.length === 0;
                }

                wad_input.addEventListener('change', () => {
                    if (wad_input.files.length > MAX_FILES) {
                        alert(`Please select at most ${MAX_FILES} files.`);
                        wad_input.value = '';
                        build_file_list();
                        return;
                    }

                    build_file_list();
                });

                function create_render_card(name) {
                    const card = document.createElement('div');
                    card.className = 'render_card';

                    const title = document.createElement('p');
                    title.textContent = name;

                    const progress = document.createElement('div');
                    progress.className = 'progress_text';
                    progress.textContent = 'queued';

                    const video_container = document.createElement('div');
                    video_container.style.display = 'none';

                    const video_player = document.createElement('video');
                    video_player.controls = true;

                    const download_link = document.createElement('a');
                    download_link.textContent = 'Download video';

                    video_container.appendChild(video_player);
                    video_container.appendChild(document.createElement('br'));
                    video_container.appendChild(download_link);

                    card.appendChild(title);
                    card.appendChild(progress);
                    card.appendChild(video_container);
                    renders.appendChild(card);

                    return { progress, video_container, video_player, download_link };
                }

                function poll_render(render_id, ui) {
                    const interval = setInterval(async () => {
                        const res = await fetch("/api/check_banner_status", {
                            method: "POST",
                            headers: {
                                "Content-Type": "application/json"
                            },
                            body: JSON.stringify({
                                id: render_id
                            })
                        });

                        const json = await res.json();

                        if (!json || !json.status) {
                            clearInterval(interval);
                            ui.progress.textContent = "failure. sorry :(";
                            ui.video_container.style.display = "none";
                            ui.video_player.style.display = "none";
                            return;
                        }

                        const status = json.status;

                        if (status === "finished") {
                            clearInterval(interval);
                            ui.progress.textContent = "finished :O";

                            ui.video_player.src = json.download_mp4;
                            ui.video_player.load();

                            ui.video_container.style.display = "block";
                            ui.download_link.href = json.download_mp4;
                        } else if (status === "processing") {
                            ui.progress.textContent = "working";
                        }

                        if (status === "error" || json.error) {
                            clearInterval(interval);
                            ui.progress.textContent = "failure. sorry :(";
                            ui.video_container.style.display = "none";
                            ui.video_player.style.display = "none";
                        }
                    }, 2500);
                }

                async function load_recently_rendered() {
                    const container = document.getElementById('recently_rendered');

                    try {
                        const res = await fetch('/api/recent');
                        const items = await res.json();

                        if (!Array.isArray(items) || items.length === 0) {
                            container.innerHTML = '<p>Nothing rendered yet.</p>';
                            return;
                        }

                        container.innerHTML = '';

                        for (const item of items) {
                            const card = document.createElement('div');
                            card.className = 'render_card';

                            const title = document.createElement('p');
                            title.textContent = item.filename || 'render';

                            const video = document.createElement('video');
                            video.src = item.download_mp4;
                            video.preload = 'metadata';

                            video.addEventListener('mouseenter', () => {
                                video.play().catch(() => {});
                            });

                            video.addEventListener('mouseleave', () => {
                                video.pause();
                                video.currentTime = 0;
                            });

                            video.addEventListener('click', () => {
                                const link = document.createElement('a');
                                link.href = item.download_mp4;
                                link.download = item.filename || 'render.mp4';
                                document.body.appendChild(link);
                                link.click();
                                link.remove();
                            });

                            card.appendChild(title);
                            card.appendChild(video);
                            container.appendChild(card);
                        }
                    } catch (e) {
                        container.innerHTML = '<p>Could not load recent renders.</p>';
                    }
                }

                load_recently_rendered();

                const recentlyRendered = document.getElementById('recently_rendered');

                recentlyRendered.addEventListener('wheel', (e) => {
                    if (e.deltaY === 0) return;

                    e.preventDefault();
                    recentlyRendered.scrollLeft += e.deltaY;
                }, { passive: false });

                document.querySelector('.carousel_left').addEventListener('click', () => {
                    document.getElementById('recently_rendered').scrollBy({ left: -300, behavior: 'smooth' });
                });

                document.querySelector('.carousel_right').addEventListener('click', () => {
                    document.getElementById('recently_rendered').scrollBy({ left: 300, behavior: 'smooth' });
                });

                form.addEventListener('submit', async (e) => {
                    e.preventDefault();
                    renders.innerHTML = '';

                    if (selected_files.length === 0) {
                        return;
                    }

                    if (selected_files.length > MAX_FILES) {
                        alert(`Please select at most ${MAX_FILES} files.`);
                        return;
                    }

                    const jobs = [];
                    for (const { file, icon_checkbox, both_checkbox, hidden_checkbox } of selected_files) {
                        const hidden = hidden_checkbox.checked;

                        if (both_checkbox.checked) {
                            jobs.push({ file, icon: true, hidden, label: `${file.name} (icon)` });
                            jobs.push({ file, icon: false, hidden, label: `${file.name} (banner)` });
                        } else {
                            jobs.push({ file, icon: icon_checkbox.checked, hidden, label: file.name });
                        }
                    }

                    jobs.forEach(job => {
                        job.ui = create_render_card(job.label);
                        job.ui.progress.textContent = 'uploading file';
                    });

                    await Promise.all(jobs.map(async (job) => {
                        const form_data = new FormData();
                        form_data.append('icon', job.icon ? '1' : '0');
                        form_data.append('hidden', job.hidden ? '1' : '0');
                        form_data.append('wad', job.file, job.file.name);

                        try {
                            const res = await fetch('/api/render-banner', {
                                method: 'POST',
                                body: form_data
                            });

                            const data = await res.json();
                            const render_id = data.render_ids && data.render_ids[0];

                            if (!render_id) {
                                job.ui.progress.textContent = 'failure. sorry :(';
                                return;
                            }

                            poll_render(render_id, job.ui);
                        } catch (e) {
                            job.ui.progress.textContent = 'failure. sorry :(';
                        }
                    }));
                });

                </script>
        </body>
        </html>
        )";

    resp.body = netkit::body::make_body<netkit::body::async_buffer_body>(string);

    co_return resp;
}

netkit::io::task<netkit::http::server::async_response>
get_banner_renderer_style(const netkit::http::server::async_request& req) {
    netkit::http::server::async_response resp;
    resp.content_type = "text/css";
    resp.http_status = 200;

    auto string = R"(
        @import url('https://fonts.googleapis.com/css2?family=Open+Sans:ital,wght@0,300..800;1,300..800&display=swap');

        :root {
            --bg: #222222;
            --card-bg: #252525;
            --border: #343840;
            --text: #e6e8ec;
            --darker-text: #9aa1ad;
            --link: #38afff;
            --button: #e6e8ec;
            --button-text: #222222;
            --button-hover: #a6a8ac;
            --radius: 10px;
        }

        * {
            box-sizing: border-box;
        }

        body {
            margin: 0;
            padding: 48px 20px;
            background: var(--bg);
            color: var(--text);
            font-family: "Open Sans", sans-serif;
            line-height: 1.55;
        }

        .container {
            max-width: 640px;
            margin: 0 auto;
        }

        h1 {
            font-size: 1.75rem;
            margin: 0 0 8px;
        }

        p {
            color: var(--darker-text);
            margin: 0 0 8px;
        }

        a {
            color: var(--link);
            text-decoration: none;
        }

        a:hover {
            text-decoration: underline;
        }

        #upload_form {
            background: var(--card-bg);
            border: 1px solid var(--border);
            border-radius: var(--radius);
            padding: 20px;
            margin: 24px 0;
            box-shadow: 0 1px 2px rgba(16, 24, 40, 0.04);
        }

        input[type="file"] {
            display: block;
            width: 100%;
            padding: 12px;
            border: 1px dashed var(--border);
            border-radius: 8px;
            background: var(--bg);
            color: var(--text);
            margin-bottom: 14px;
            font-size: 0.9rem;
        }

        #file_list:not(:empty) {
            border: 1px solid var(--border);
            border-radius: 8px;
            margin-bottom: 16px;
            overflow: hidden;
        }

        .file_row {
            display: flex;
            align-items: center;
            gap: 12px;
            padding: 10px 14px;
            border-bottom: 1px solid var(--border);
            background: var(--card-bg);
        }

        .file_row:last-child {
            border-bottom: none;
        }

        .file_row .file_name {
            flex: 1;
            font-size: 0.9rem;
            overflow: hidden;
            text-overflow: ellipsis;
            white-space: nowrap;
        }

        .file_row label {
            display: flex;
            align-items: center;
            gap: 6px;
            font-size: 0.85rem;
            color: var(--darker-text);
            cursor: pointer;
            user-select: none;
        }

        button[type="submit"] {
            background: var(--button);
            color: var(--button-text);
            border: none;
            padding: 10px 22px;
            border-radius: 8px;
            font-size: 0.95rem;
            font-weight: 600;
            cursor: pointer;
            transition: background 0.15s ease;
        }

        button[type="submit"]:hover:not(:disabled) {
            background: var(--button-hover);
        }

        button[type="submit"]:disabled {
            background: var(--border);
            color: var(--darker-text);
            cursor: not-allowed;
        }

        .render_card {
            background: var(--card-bg);
            border: 1px solid var(--border);
            border-radius: var(--radius);
            padding: 16px 18px;
            margin-bottom: 12px;
            box-shadow: 0 1px 2px rgba(16, 24, 40, 0.04);
        }

        .render_card p {
            color: var(--text);
            font-weight: 600;
            margin: 0 0 6px;
        }

        .render_card .progress_text {
            color: var(--darker-text);
            font-size: 0.85rem;
            margin-bottom: 10px;
        }

        .render_card video {
            max-width: 100%;
            border-radius: 8px;
            display: block;
            margin-bottom: 8px;
            background: #000;
        }

        .render_card a {
            font-size: 0.85rem;
            font-weight: 600;
        }

        .carousel {
            display: flex;
            align-items: center;
            gap: 8px;
            margin-bottom: 12px;
        }

        .carousel_track {
            display: flex;
            gap: 12px;
            overflow-x: auto;
            scroll-behavior: smooth;
            padding-bottom: 4px;
            flex: 1;
            scrollbar-width: none;
        }

        html::-webkit-scrollbar {
            display: none;
        }

        .carousel_track .render_card {
            flex: 0 0 220px;
            margin-bottom: 0;
        }

        .carousel_btn {
            flex: 0 0 auto;
            width: 32px;
            height: 32px;
            border-radius: 50%;
            border: 1px solid var(--border);
            background: var(--card-bg);
            color: var(--text);
            font-size: 1.1rem;
            line-height: 1;
            cursor: pointer;
            display: flex;
            align-items: center;
            justify-content: center;
            transition: background 0.15s ease;
        }

        .carousel_btn:hover {
            background: var(--border);
        }
        )";

    resp.body = netkit::body::make_body<netkit::body::async_buffer_body>(string);

    co_return resp;
}

std::string sanitize_filename(const std::string& input) {
    std::string result;
    result.reserve(input.size());

    const std::string invalid_chars = R"(<>:"/\|?*)";

    for (char c : input) {
        if (static_cast<unsigned char>(c) < 32) {
            continue;
        }

        if (invalid_chars.find(c) != std::string::npos) {
            result += '_';
        } else {
            result += c;
        }
    }

    while (!result.empty() && (result.back() == ' ' || result.back() == '.')) {
        result.pop_back();
    }

    while (!result.empty() && result.front() == ' ') {
        result.erase(result.begin());
    }

    if (result.empty()) {
        result = "file";
    }

    static const std::unordered_set<std::string> reserved = {
        "CON","PRN","AUX","NUL",
        "COM1","COM2","COM3","COM4","COM5","COM6","COM7","COM8","COM9",
        "LPT1","LPT2","LPT3","LPT4","LPT5","LPT6","LPT7","LPT8","LPT9"
    };

    std::string upper = result;
    std::ranges::transform(upper, upper.begin(), [](unsigned char c) {
        return std::toupper(c);
    });

    if (reserved.contains(upper)) {
        result = "_" + result;
    }

    constexpr size_t max_length = 255;
    if (result.size() > max_length) {
        result = result.substr(0, max_length);
    }

    return result;
}

netkit::io::task<std::optional<std::string>>
save_and_start_render(netkit::http::utility::async_multipart_part& part, bool add_icon, bool hidden) {
    const std::string key = generate_random_string(32);

    std::filesystem::path wd = TEMP_DIRECTORY + "/" + key + "/";
    std::string sanitized_name = sanitize_filename(part.filename);
    std::filesystem::path output_file = wd.string() + sanitized_name;

    if (!std::filesystem::is_directory(wd)) {
        std::filesystem::create_directories(wd);
    }

    streaming_hasher hasher;

    {
        std::ofstream of{output_file, std::ofstream::binary};

        char buffer[8192];
        while (true) {
            auto result = co_await part.data->read(buffer, sizeof(buffer));

            if (result.get_bytes_read() > 0) {
                of.write(buffer, static_cast<long>(result.get_bytes_read()));
                hasher.update(buffer, result.get_bytes_read());
            }

            if (result.get_status() == netkit::body::read_status::eof) {
                break;
            }

            if (result.get_status() == netkit::body::read_status::error) {
                throw std::runtime_error{"encountered an error"};
            }
        }
    }

    if (!std::filesystem::exists(output_file)) {
        co_return std::nullopt;
    }

    std::string content_hash = hasher.hex_digest();

    {
        std::lock_guard<std::mutex> lock(banner_trackers_mutex);

        for (auto &tracker: banner_trackers | std::views::values) {
            if (tracker.render_status.load() != status::finished) continue;
            if (tracker.content_hash != content_hash) continue;
            if (tracker.icon != add_icon) continue;
            if (!std::filesystem::is_regular_file(tracker.actual_filename)) continue;

            std::error_code ec;
            std::filesystem::remove_all(wd, ec);

            banner_tracker cache_hit;
            cache_hit.key = key;
            cache_hit.actual_filename = tracker.actual_filename;
            cache_hit.original_filename = sanitized_name;
            cache_hit.content_hash = content_hash;
            cache_hit.icon = add_icon;
            cache_hit.hidden = hidden;
            cache_hit.uploaded_at = now_unix_seconds();
            cache_hit.render_status = status::finished;

            banner_trackers[key] = cache_hit;
            save_renders_index_locked();

            co_return key;
        }
    }

    std::string output_video = output_file.string();
    auto ext = output_video.find_last_of('.');
    if (ext != std::string::npos) {
        output_video = output_video.substr(0, ext);
    }
    output_video += ".mp4";

    {
        std::lock_guard<std::mutex> lock(banner_trackers_mutex);

        auto [it, inserted] = banner_trackers.try_emplace(key);
        auto& _tracker = it->second;

        _tracker.key = key;
        _tracker.actual_filename = output_video;
        _tracker.original_filename = sanitized_name;
        _tracker.content_hash = content_hash;
        _tracker.icon = add_icon;
        _tracker.hidden = hidden;
        _tracker.uploaded_at = now_unix_seconds();
        _tracker.render_status = status::processing;

        save_renders_index_locked();
    }

    std::thread([output_file, output_video, key, add_icon]() {
        std::string cmd =
            "wbr \"" +
            output_file.string() +
            "\" -o \"" +
            output_video + "\"";

        if (add_icon) {
            cmd += " --icon";
        }

        std::system(cmd.c_str());

        {
            std::lock_guard<std::mutex> lock(banner_trackers_mutex);

            auto it = banner_trackers.find(key);
            if (it != banner_trackers.end()) {
                it->second.render_status = status::finished;
                save_renders_index_locked();
            }
        }
    }).detach();

    co_return key;
}

netkit::io::task<std::vector<std::string>>
render_banners(const netkit::http::server::async_request& req) {
    std::string boundary{};

    if (req.headers.contains("content-type")) {
        std::string content_type = req.headers.at("content-type");
        boundary = netkit::http::utility::extract_boundary(content_type);
    }

    netkit::http::utility::async_multipart_reader reader{*req.body, boundary};
    netkit::http::utility::async_multipart_part part;

    std::vector<std::string> keys;

    bool pending_icon = false;
    bool pending_hidden = false;

    while (keys.size() < MAX_FILES_PER_REQUEST && co_await reader.next(part)) {
        if (part.filename.empty()) {
            std::string value;
            char buffer[256];
            while (true) {
                auto result = co_await part.data->read(buffer, sizeof(buffer));

                if (result.get_bytes_read() > 0) {
                    value.append(buffer, result.get_bytes_read());
                }

                if (result.get_status() == netkit::body::read_status::eof) {
                    break;
                }

                if (result.get_status() == netkit::body::read_status::error) {
                    throw std::runtime_error{"encountered an error"};
                }
            }

            if (part.name == "hidden") {
                pending_hidden = (value == "1");
            } else {
                pending_icon = (value == "1");
            }
            continue;
        }

        auto key = co_await save_and_start_render(part, pending_icon, pending_hidden);
        pending_icon = false;
        pending_hidden = false;

        if (key.has_value()) {
            keys.push_back(*key);
        }
    }

    if (keys.empty()) {
        std::cerr << "No multipart\n";
    }

    co_return keys;
}

netkit::io::task<netkit::http::server::async_response>
get_render_banner(const netkit::http::server::async_request& req) {
    netkit::http::server::async_response resp;
    resp.content_type = "application/json";

    nlohmann::json ret;

    if (req.method != "POST") {
        ret["error"] = "Invalid method";
        resp.http_status = 400;
        resp.body = netkit::body::make_body<netkit::body::async_buffer_body>(ret.dump());
        co_return resp;
    }

    std::vector<std::string> keys = co_await render_banners(req);

    if (keys.empty()) {
        ret["error"] = "No files uploaded";
        resp.http_status = 400;
        resp.body = netkit::body::make_body<netkit::body::async_buffer_body>(ret.dump());
        co_return resp;
    }

    ret["render_ids"] = keys;

    resp.body = netkit::body::make_body<netkit::body::async_buffer_body>(ret.dump());

    co_return resp;
}

netkit::io::task<netkit::http::server::async_response>
get_banner_index(const netkit::http::server::async_request& req) {
    netkit::http::server::async_response resp;
    resp.content_type = "application/json";

    std::string key = std::filesystem::path(req.endpoint).filename().string();

    std::string filename;

    {
        std::lock_guard<std::mutex> lock(banner_trackers_mutex);

        auto it = banner_trackers.find(key);
        if (it == banner_trackers.end()) {
            nlohmann::json ret;
            ret["error"] = "Invalid request or server error";
            resp.http_status = 400;
            resp.body = netkit::body::make_body<netkit::body::async_buffer_body>(ret.dump());
            co_return resp;
        }

        filename = it->second.actual_filename;
    }

    if (!std::filesystem::is_regular_file(filename)) {
        nlohmann::json ret;
        ret["error"] = "Invalid request or server error";
        resp.http_status = 400;
        resp.body = netkit::body::make_body<netkit::body::async_buffer_body>(ret.dump());
        co_return resp;
    }

    resp.body = netkit::body::make_body<netkit::body::async_file_body>(filename);
    resp.content_type = "video/mp4";
    resp.http_status = 200;

    co_return resp;
}

netkit::io::task<netkit::http::server::async_response>
get_recent_renders(const netkit::http::server::async_request& req) {
    netkit::http::server::async_response resp;
    resp.content_type = "application/json";
    resp.http_status = 200;

    struct entry {
        std::string key;
        std::string filename;
        std::int64_t uploaded_at;
    };

    std::vector<entry> entries;

    {
        std::lock_guard<std::mutex> lock(banner_trackers_mutex);

        for (const auto& [key, tracker] : banner_trackers) {
            if (tracker.hidden) continue;
            if (tracker.render_status.load() != status::finished) continue;
            if (!std::filesystem::is_regular_file(tracker.actual_filename)) continue;

            entries.push_back({key, tracker.original_filename, tracker.uploaded_at});
        }
    }

    std::ranges::sort(entries, [](const entry& a, const entry& b) {
        return a.uploaded_at > b.uploaded_at;
    });

    if (entries.size() > MAX_RECENT_RENDERS) {
        entries.resize(MAX_RECENT_RENDERS);
    }

    nlohmann::json ret = nlohmann::json::array();
    for (const auto& e : entries) {
        ret.push_back({
            {"filename", e.filename},
            {"uploaded_at", e.uploaded_at},
            {"download_mp4", "/get/" + e.key},
        });
    }

    resp.body = netkit::body::make_body<netkit::body::async_buffer_body>(ret.dump());
    co_return resp;
}

netkit::io::task<netkit::http::server::async_response>
check_banner_status(const netkit::http::server::async_request& req, std::optional<std::size_t> len) {
    netkit::http::server::async_response resp;
    resp.content_type = "application/json";

    nlohmann::json ret;
    nlohmann::json in;

    if (req.method != "POST") {
        ret["error"] = "Invalid method";
        resp.http_status = 400;
        resp.body = netkit::body::make_body<netkit::body::async_buffer_body>(ret.dump());
        co_return resp;
    }

    if (req.content_type != "application/json") {
        ret["error"] = "Invalid content type";
        resp.http_status = 400;
        resp.body = netkit::body::make_body<netkit::body::async_buffer_body>(ret.dump());
        co_return resp;
    }

    try {
        auto data = co_await req.body->read_all(len);
        std::cout << data << "\n";
        in = nlohmann::json::parse(data);
    } catch (std::exception& e) {
        std::cerr << e.what() << "\n";

        ret["error"] = "Invalid JSON";
        resp.http_status = 400;
        resp.body = netkit::body::make_body<netkit::body::async_buffer_body>(ret.dump());
        co_return resp;
    }

    if (in.contains("id") == false || in.at("id").is_string() == false) {
        ret["error"] = "Invalid JSON (no id)";
        resp.http_status = 400;
        resp.body = netkit::body::make_body<netkit::body::async_buffer_body>(ret.dump());
        co_return resp;
    }

    std::string id = in["id"].get<std::string>();
    if (id.empty()) {
        ret["error"] = "Invalid JSON";
        resp.http_status = 400;
        resp.body = netkit::body::make_body<netkit::body::async_buffer_body>(ret.dump());
        co_return resp;
    }

    banner_tracker tracker;

    {
        std::lock_guard<std::mutex> lock(banner_trackers_mutex);

        auto it = banner_trackers.find(id);
        if (it == banner_trackers.end()) {
            ret["error"] = "Invalid ID or no render started";
            resp.http_status = 400;
            resp.body = netkit::body::make_body<netkit::body::async_buffer_body>(ret.dump());
            co_return resp;
        }

        tracker = it->second;
    }

    switch (tracker.render_status) {
        case status::processing:
            ret["status"] = "processing";
            resp.http_status = 200;
            resp.body = netkit::body::make_body<netkit::body::async_buffer_body>(ret.dump());
            co_return resp;
        case status::finished:
            ret["status"] = "finished";
            resp.http_status = 200;
            break;
        default: break;
    }

    if (!std::filesystem::is_regular_file(tracker.actual_filename)) {
        ret["error"] = "Server error (file doesn't exist)";
        resp.http_status = 500;
        resp.body = netkit::body::make_body<netkit::body::async_buffer_body>(ret.dump());
        co_return resp;
    }

    ret["download_mp4"] = "/get/" + tracker.key;

    resp.body = netkit::body::make_body<netkit::body::async_buffer_body>(ret.dump());
    co_return resp;
}

netkit::io::task<void> run_server(netkit::io::io_context& ctx) {
    netkit::http::server::async_server server(
    ctx,
    netkit::http::server::server_settings{
        .port = PORT,
        .enable_session = false,
        .trust_x_forwarded_for = true,
    },
    [&](const netkit::http::server::async_request& req) -> netkit::io::task<netkit::http::server::async_response> {
        std::cout << "Received request from: " << req.ip_address << "\n"
                  << "Endpoint: " << req.endpoint << "\n"
                  << "Method: " << req.method << "\n"
                  << "User-Agent: " << req.user_agent << "\n";

        auto endpoint = req.endpoint;

        // trim trailing
        if (endpoint.back() == '/') {
            endpoint.pop_back();
        }

        std::size_t len = 0;

        for (auto& it : req.headers) {
            std::cout << it.first << ": " << it.second << "\n";
        }

        if (req.headers.contains("content-length")) {
            const auto& value = req.headers.at("content-length");

            auto [ptr, ec] = std::from_chars(
                value.data(),
                value.data() + value.size(),
                len
            );

            if (ec != std::errc{} || ptr != value.data() + value.size()) {
                netkit::http::server::async_response resp;
                resp.http_status = 400;
                resp.content_type = "text/plain";
                resp.body = netkit::body::make_body<netkit::body::async_buffer_body>(std::string("400: Invalid Content-Length header"));
                co_return resp;
            }

            if (len > MAX_REQUEST_SIZE) {
                netkit::http::server::async_response resp;
                resp.http_status = 413;
                resp.content_type = "text/plain";
                resp.body = netkit::body::make_body<netkit::body::async_buffer_body>(std::string("413: Payload too large"));
                co_return resp;
            }
        }

        if (endpoint.empty()) {
            co_return co_await get_index(req);
        } else if (endpoint == "/banner-renderer") {
            co_return co_await get_banner_renderer_index(req);
        } else if (endpoint == "/banner-renderer/style.css") {
            co_return co_await get_banner_renderer_style(req);
        } else if (endpoint == "/api/render-banner") {
            co_return co_await get_render_banner(req);
        } else if (endpoint == "/api/check_banner_status") {
            if (!len && req.headers.contains("Connection") && req.headers.at("Connection") != "close") {
                netkit::http::server::async_response resp;
                resp.http_status = 400;
                resp.content_type = "text/plain";
                resp.body = netkit::body::make_body<netkit::body::async_buffer_body>(std::string("400: Invalid Content-Length header"));
                co_return resp;
            }
            co_return co_await check_banner_status(req, len);
        } else if (endpoint == "/api/recent") {
            co_return co_await get_recent_renders(req);
        } else if (endpoint.starts_with("/get")) {
            co_return co_await get_banner_index(req);
        }

        netkit::http::server::async_response resp;
        resp.http_status = 404;
        resp.body = netkit::body::make_body<netkit::body::async_buffer_body>(std::string("404: Not found here. Oops.\n"));

        co_return resp;
    });

    co_await server.run();
    co_return;
}

int main(int argc, char** argv) {
    std::filesystem::create_directories(TEMP_DIRECTORY);

    load_renders_index();
    std::thread(cleanup_old_renders).detach();

    netkit::io::io_context ctx;

    std::cout << "Server started on port " << PORT << std::endl;

    ctx.spawn(run_server(ctx));
    ctx.run();

    return EXIT_SUCCESS;
}
