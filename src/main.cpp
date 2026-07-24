#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <unordered_set>
#include <netkit/http/sync_server.hpp>
#include <netkit/body/file_body.hpp>
#include <netkit/body/buffer_body.hpp>
#include <netkit/http/multipart.hpp>
#include <netkit/http/multipart_reader.hpp>
#include <nlohmann/json.hpp>

constexpr int PORT = 8080;
const std::string TEMP_DIRECTORY = "/tmp/wii-banner-renderer";

enum class status {
    processing,
    finished
};

struct banner_tracker {
    std::string key{};
    std::atomic<status> render_status;
    std::string actual_filename{};

    // we are using this struct so we can expand it later as needed
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

netkit::http::server::response
get_index(const netkit::http::server::request& req) {
    netkit::http::server::response resp;
    resp.content_type = "text/html";
    resp.http_status = 200;

    auto string = R"(
        <!DOCTYPE html>
        <html>
        <head>
            <title>Wii Web Tools</title>
            <meta charset="UTF-8">
        </head>
        <body>
            <h1>Wii Web Tools by Forwarder Factory</h1>
                <p>This website provides some useful Wii tools right in your web browser.</p>
                <ul>
                    <li><a href="/banner-renderer/">Banner Renderer</a> - Render your Wii channels to a video file</li>
                    <li>More tools coming later.</li>
                </ul>
        </body>
        </html>
        )";

    resp.body = netkit::body::make_body<netkit::body::buffer_body>(string);

    return resp;
}

netkit::http::server::response
get_banner_renderer_index(const netkit::http::server::request& req) {
    netkit::http::server::response resp;
    resp.content_type = "text/html";
    resp.http_status = 200;

    auto string = R"(
        <!DOCTYPE html>
        <html>
        <head>
            <title>Wii Banner Renderer</title>
            <meta charset="UTF-8">
            <style>
                #video_container {
                    display: none;
                }
            </style>
        </head>
        <body>
            <h1>Wii Banner Renderer</h1>
                <p>Render your Wii channels into playable video files in your browser.</p>
                <p>For more options, check out the <a href="https://github.com/ForwarderFactory/wii-banner-renderer">wii-banner-renderer</a> program.</p>

                <form id="upload_form" method="POST" enctype="multipart/form-data">
                    <input type="file" name="wad" accept=".wad" required>
                    <button type="submit">Render</button>
                </form>

                <div id="progress"></div>
                <div id="video_container">
                    <video id="video_player" controls></video>
                    <br>
                    <a id="download_link">Download video</a>
                </div>

                <script>
                const form = document.getElementById('upload_form');
                const progress = document.getElementById('progress');
                const video_container = document.getElementById('video_container');
                const video_player = document.getElementById('video_player');
                const download_link = document.getElementById('download_link');

                function poll_render(render_id) {
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
                            progress.textContent = "failure. sorry :(";
                            return
                        }

                        const status = json.status;

                        if (status === "finished") {
                            clearInterval(interval);
                            progress.textContent = "finished :O";

                            video_player.src = json.download_webm;
                            video_player.load();

                            video_container.style.display = "block";

                            download_link.href = json.download_webm;
                        } else if (status === "processing") {
                            progress.textContent = "working";
                        }

                        if (status === "error" || json.error) {
                            clearInterval(interval);
                            progress.textContent = "failure. sorry :(";
                        }
                    }, 2500);
                }

                form.addEventListener('submit', async (e) => {
                    e.preventDefault();
                    progress.textContent = "uploading file";

                    const form_data = new FormData(form);
                    const res = await fetch('/api/render-banner', {
                        method: 'POST',
                        body: form_data
                    });

                    const data = await res.json();

                    if (!data.render_id) {
                        progress.textContent = "failure. sorry :(";
                        return;
                    }

                    const render_id = data.render_id;
                    poll_render(render_id);
                });

                </script>
        </body>
        </html>
        )";

    resp.body = netkit::body::make_body<netkit::body::buffer_body>(string);

    return resp;
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

void render_banner(const netkit::http::server::request& req, const std::string& key) {
    std::string boundary{};

    if (req.headers.find("Content-Type") != req.headers.end()) {
	    std::string content_type = req.headers.at("Content-Type");
	    boundary = netkit::http::utility::extract_boundary(content_type);
    }

    netkit::http::utility::multipart_reader reader{*req.body, boundary};
    netkit::http::utility::multipart_part part;

    std::filesystem::path wd;
    std::filesystem::path output_file;

    if (reader.next(part)) {
    	wd = TEMP_DIRECTORY + "/" + key + "/";
    	output_file = wd.string() + sanitize_filename(part.filename);

	if (!std::filesystem::is_directory(wd)) {
        	std::filesystem::create_directories(wd);
    	}

	std::ofstream of{output_file, std::ofstream::binary};

	char buffer[8192];
	while (true) {
		auto result = part.data->read(buffer, sizeof(buffer));

		if (result.get_bytes_read() > 0) {
			of.write(buffer, result.get_bytes_read());
		}

		if (result.get_status() == netkit::body::read_status::eof) {
			break;
		}

		if (result.get_status() == netkit::body::read_status::error) {
			throw std::runtime_error{"encountered an error"};
		}
	}
    }

    std::string output_video = output_file;
    auto ext = output_video.find_last_of('.');
    if (ext != std::string::npos) {
        output_video = output_video.substr(0, ext);
    }

    output_video += ".webm";

    banner_tracker tracker;
    tracker.render_status = status::processing;
    tracker.key = key;
    tracker.actual_filename = output_video;

    {
        std::lock_guard<std::mutex> lock(banner_trackers_mutex);

        auto [it, inserted] = banner_trackers.try_emplace(key);
        auto& _tracker = it->second;

        _tracker.key = key;
        _tracker.actual_filename = output_video;
        _tracker.render_status = status::processing;
    }

    std::thread([=]() {
        std::string cmd =
            "wii-banner-renderer -webm \"" +
            output_file.string() +
            "\" -o \"" +
            output_video + "\"";

        std::system(cmd.c_str());

        {
            std::lock_guard<std::mutex> lock(banner_trackers_mutex);

            auto it = banner_trackers.find(key);
            if (it != banner_trackers.end()) {
                it->second.render_status = status::finished;
            }
        }
    }).detach();
}

netkit::http::server::response get_render_banner(const netkit::http::server::request& req) {
    netkit::http::server::response resp;
    resp.content_type = "application/json";

    nlohmann::json ret;

    if (req.method != "POST") {
        ret["error"] = "Invalid method";
        resp.http_status = 400;
    	resp.body = netkit::body::make_body<netkit::body::buffer_body>(ret.dump());
        return resp;
    }

    const std::string key = generate_random_string(32);

    ret["render_id"] = key;

    render_banner(req, key);

    resp.body = netkit::body::make_body<netkit::body::buffer_body>(ret.dump());

    return resp;
}

netkit::http::server::response
get_banner_index(const netkit::http::server::request& req) {
    netkit::http::server::response resp;

    resp.content_type = "application/json";

    std::string key = std::filesystem::path(req.endpoint).filename().string();
    if (!banner_trackers.contains(key)) {
        nlohmann::json ret;
        ret["error"] = "Invalid request or server error";
        resp.http_status = 400;
        resp.body = netkit::body::make_body<netkit::body::buffer_body>(ret.dump());
        return resp;
    }


    const banner_tracker& tracker = banner_trackers[key];
    if (!std::filesystem::is_regular_file(tracker.actual_filename)) {
        nlohmann::json ret;
        ret["error"] = "Invalid request or server error";
        resp.http_status = 400;
        resp.body = netkit::body::make_body<netkit::body::buffer_body>(ret.dump());
        return resp;
    }

    resp.body = netkit::body::make_body<netkit::body::file_body>(tracker.actual_filename);

    resp.content_type = "video/webm";
    resp.http_status = 200;

    return resp;
}

netkit::http::server::response
check_banner_status(const netkit::http::server::request& req) {
    netkit::http::server::response resp;
    resp.content_type = "application/json";

    nlohmann::json ret;
    nlohmann::json in;

    if (req.method != "POST") {
        ret["error"] = "Invalid method";
        resp.http_status = 400;
        resp.body = netkit::body::make_body<netkit::body::buffer_body>(ret.dump());
        return resp;
    }

    if (req.content_type != "application/json") {
        ret["error"] = "Invalid content type";
        resp.http_status = 400;
        resp.body = netkit::body::make_body<netkit::body::buffer_body>(ret.dump());
        return resp;
    }

    try {
        in = nlohmann::json::parse(req.body->read_all());
    } catch (std::exception& e) {
	std::cerr << e.what() << "\n";
        ret["error"] = "Invalid JSON";
        resp.http_status = 400;
        resp.body = netkit::body::make_body<netkit::body::buffer_body>(ret.dump());
        return resp;
    }

    if (in.contains("id") == false || in.at("id").is_string() == false) {
        ret["error"] = "Invalid JSON (no id)";
        resp.http_status = 400;
        resp.body = netkit::body::make_body<netkit::body::buffer_body>(ret.dump());
        return resp;
    }

    std::string id = in["id"].get<std::string>();
    if (id.empty()) {
        ret["error"] = "Invalid JSON";
        resp.http_status = 400;
        resp.body = netkit::body::make_body<netkit::body::buffer_body>(ret.dump());
        return resp;
    }

    if (!banner_trackers.contains(id)) {
        ret["error"] = "Invalid ID or no render started";
        resp.http_status = 400;
        resp.body = netkit::body::make_body<netkit::body::buffer_body>(ret.dump());
        return resp;
    }

    const banner_tracker& tracker = banner_trackers.at(id);
    switch (tracker.render_status) {
        case status::processing:
            ret["status"] = "processing";
            resp.http_status = 200;
            resp.body = netkit::body::make_body<netkit::body::buffer_body>(ret.dump());
            return resp;
        case status::finished:
            ret["status"] = "finished";
            resp.http_status = 200;
            break;
        default: break;
    }

    if (!std::filesystem::is_regular_file(tracker.actual_filename)) {
        ret["error"] = "Server error (file doesn't exist)";
        resp.http_status = 500;
        resp.body = netkit::body::make_body<netkit::body::buffer_body>(ret.dump());
        return resp;
    }

    ret["download_webm"] = "/get/" + tracker.key;

    resp.body = netkit::body::make_body<netkit::body::buffer_body>(ret.dump());
    return resp;
}

int main(int argc, char** argv) {
    netkit::http::server::sync_server server(
        netkit::http::server::server_settings{
        .port = PORT,
        .enable_session = false,
        .trust_x_forwarded_for = true,
        },
        [&](const netkit::http::server::request& req) -> netkit::http::server::response {
            std::cout << "Received request from: " << req.ip_address << "\n"
                      << "Endpoint: " << req.endpoint << "\n"
                      << "Method: " << req.method << "\n"
                      << "User-Agent: " << req.user_agent << "\n";
            //          << "Body: " << req.body << "\n";

            auto endpoint = req.endpoint;

            // trim trailing
            if (endpoint.back() == '/') {
                endpoint.pop_back();
            }

            if (endpoint.empty()) {
                return get_index(req);
            } else if (endpoint == "/banner-renderer") {
                return get_banner_renderer_index(req);
            } else if (endpoint == "/api/render-banner") {
                auto response = get_render_banner(req);
                std::cout << response.body << std::endl;
                return response;
            } else if (endpoint == "/api/check_banner_status") {
                return check_banner_status(req);
            } else if (endpoint.starts_with("/get")) {
                return get_banner_index(req);
            }

            netkit::http::server::response resp;

            resp.http_status = 404;
    	    resp.body = netkit::body::make_body<netkit::body::buffer_body>(std::string("404: Not found here. Oops.\n"));

            return resp;
        });

    std::cout << "Server started on port " << PORT << std::endl;

    server.run();

    return EXIT_SUCCESS;
}
