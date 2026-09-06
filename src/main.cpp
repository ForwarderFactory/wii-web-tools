#include <iostream>
#include <mutex>
#include <random>
#include <string>
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
constexpr std::size_t MAX_FILES_PER_REQUEST = 5;
constexpr std::size_t MAX_REQUEST_SIZE = 128000000;

enum class status {
    processing,
    finished
};

struct banner_tracker {
    std::string key{};
    std::atomic<status> render_status;
    std::string actual_filename{};

    banner_tracker& operator=(const banner_tracker& other) {
        key = other.key;
        actual_filename = other.actual_filename;
        render_status.store(other.render_status.load());
        return *this;
    }
};

std::unordered_map<std::string, banner_tracker> banner_trackers;
std::mutex banner_trackers_mutex;

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

                        row.appendChild(name);
                        row.appendChild(label);
                        file_list.appendChild(row);

                        selected_files.push({ file, icon_checkbox });
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

                    const uis = selected_files.map(({ file }) => create_render_card(file.name));
                    uis.forEach(ui => ui.progress.textContent = 'uploading file');

                    const form_data = new FormData();
                    for (const { file, icon_checkbox } of selected_files) {
                        form_data.append('icon', icon_checkbox.checked ? '1' : '0');
                        form_data.append('wad', file, file.name);
                    }

                    const res = await fetch('/api/render-banner', {
                        method: 'POST',
                        body: form_data
                    });

                    const data = await res.json();

                    if (!data.render_ids || !Array.isArray(data.render_ids) || data.render_ids.length === 0) {
                        uis.forEach(ui => ui.progress.textContent = 'failure. sorry :(');
                        return;
                    }

                    // render_ids come back in the same order the files were appended above.
                    data.render_ids.forEach((render_id, i) => {
                        const ui = uis[i] || create_render_card(`file ${i + 1}`);
                        poll_render(render_id, ui);
                    });
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
save_and_start_render(netkit::http::utility::async_multipart_part& part, bool add_icon) {
    const std::string key = generate_random_string(32);

    std::filesystem::path wd = TEMP_DIRECTORY + "/" + key + "/";
    std::filesystem::path output_file = wd.string() + sanitize_filename(part.filename);

    if (!std::filesystem::is_directory(wd)) {
        std::filesystem::create_directories(wd);
    }

    {
        std::ofstream of{output_file, std::ofstream::binary};

        char buffer[8192];
        while (true) {
            auto result = co_await part.data->read(buffer, sizeof(buffer));

            if (result.get_bytes_read() > 0) {
                of.write(buffer, static_cast<long>(result.get_bytes_read()));
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
        _tracker.render_status = status::processing;
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

            pending_icon = (value == "1");
            continue;
        }

        auto key = co_await save_and_start_render(part, pending_icon);
        pending_icon = false;

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
    netkit::io::io_context ctx;

    std::cout << "Server started on port " << PORT << std::endl;

    ctx.spawn(run_server(ctx));
    ctx.run();

    return EXIT_SUCCESS;
}