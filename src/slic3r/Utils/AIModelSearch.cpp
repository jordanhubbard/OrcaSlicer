#include "AIModelSearch.hpp"

#include <algorithm>
#include <cctype>
#include <memory>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <nlohmann/json.hpp>

#include "libslic3r/AppConfig.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/Utils/AIProvider.hpp"
#include "slic3r/Utils/Http.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {

// Extensions Orca's Plater::load_files() can read. STEP appears as .step/.stp.
static const char *kSupportedExts[] = { ".3mf", ".stl", ".step", ".stp", ".obj" };

bool ai_model_url_is_supported(const std::string &url)
{
    // Ignore any query string / fragment when checking the extension.
    std::string path = url;
    const size_t cut = path.find_first_of("?#");
    if (cut != std::string::npos)
        path = path.substr(0, cut);
    for (const char *ext : kSupportedExts)
        if (boost::algorithm::iends_with(path, ext))
            return true;
    return false;
}

// Best-effort filename for a URL, falling back to the model title.
static std::string filename_for(const AIModelCandidate &candidate)
{
    std::string path = candidate.url;
    const size_t cut = path.find_first_of("?#");
    if (cut != std::string::npos)
        path = path.substr(0, cut);
    const size_t slash = path.find_last_of('/');
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    if (name.empty() || name.find('.') == std::string::npos) {
        // Derive one from the title + the detected extension.
        std::string ext = ".3mf";
        for (const char *e : kSupportedExts)
            if (boost::algorithm::iends_with(path, e)) { ext = e; break; }
        std::string base = candidate.title.empty() ? "model" : candidate.title;
        std::string sane;
        for (char c : base) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')
                sane += c;
            else if (c == ' ')
                sane += '_';
        }
        if (sane.empty())
            sane = "model";
        name = sane + ext;
    }
    return name;
}

AIModelSearchResult ai_search_models(const std::string &query)
{
    AIModelSearchResult out;

    AppConfig *ac = GUI::wxGetApp().app_config;
    AIConfig cfg = ac ? AIProvider::config_from_app_config(*ac) : AIConfig();
    std::unique_ptr<AIProvider> provider(AIProvider::create(cfg));
    if (! provider) {
        out.error = "No AI provider is configured. Open Preferences > General > AI and register a provider and API key first.";
        return out;
    }

    std::vector<AIMessage> messages;
    {
        AIMessage sys;
        sys.role    = "system";
        sys.content = "You help a 3D-printing app find printable models the user can download. "
                      "Return results ONLY by calling the suggest_models tool (never reply in prose). "
                      "Each result must be a real, publicly downloadable model file (.3mf, .stl, .step, or .obj) "
                      "from a well-known model repository (Printables, MakerWorld, Thingiverse, Thangs, "
                      "GrabCAD, Cults3D). Respect each site's terms of service; only link to pages/files that "
                      "allow direct download. Prefer .3mf or .stl. Return at most 8 results.";
        messages.push_back(std::move(sys));
        AIMessage user;
        user.role    = "user";
        user.content = "Find printable 3D models matching: " + query;
        messages.push_back(std::move(user));
    }

    nlohmann::json params;
    {
        nlohmann::json tool = {
            {"type", "function"},
            {"function", {
                {"name", "suggest_models"},
                {"description", "Return candidate 3D-model downloads that match the user's query."},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"results", {
                            {"type", "array"},
                            {"description", "Up to 8 candidate models."},
                            {"items", {
                                {"type", "object"},
                                {"properties", {
                                    {"title",  {{"type", "string"}, {"description", "Human-readable model name."}}},
                                    {"url",    {{"type", "string"}, {"description", "Direct download URL for a .3mf/.stl/.step/.obj file."}}},
                                    {"source", {{"type", "string"}, {"description", "Originating site, e.g. Printables."}}}
                                }},
                                {"required", nlohmann::json::array({"title", "url", "source"})}
                            }}
                        }}
                    }},
                    {"required", nlohmann::json::array({"results"})}
                }}
            }}
        };
        params["tools"]       = nlohmann::json::array({tool});
        params["tool_choice"] = {{"type", "function"}, {"function", {{"name", "suggest_models"}}}};
        params["temperature"] = 0.2;
    }

    AIResponse resp = provider->chat(messages, params);
    if (! resp.ok) {
        out.error = "Search request failed: " + resp.error;
        return out;
    }
    if (resp.tool_call_arguments.empty()) {
        out.error = "This model didn't return any results (it didn't call the suggest_models tool), "
                    "so it isn't suitable for model search. Try a recommended instruct model.";
        return out;
    }

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(resp.tool_call_arguments);
    } catch (const std::exception &e) {
        out.error = std::string("Could not parse the search results: ") + e.what();
        return out;
    }

    if (! parsed.contains("results") || ! parsed["results"].is_array()) {
        out.error = "The search returned no results.";
        return out;
    }

    for (const auto &item : parsed["results"]) {
        if (! item.is_object())
            continue;
        AIModelCandidate c;
        c.title  = item.value("title", std::string());
        c.url    = item.value("url", std::string());
        c.source = item.value("source", std::string());
        if (c.url.empty())
            continue;
        if (! (boost::algorithm::istarts_with(c.url, "http://") ||
               boost::algorithm::istarts_with(c.url, "https://")))
            continue;
        if (! ai_model_url_is_supported(c.url))
            continue;   // not a directly loadable model file
        if (c.title.empty())
            c.title = c.url;
        out.candidates.push_back(std::move(c));
    }

    if (out.candidates.empty()) {
        out.error = "No downloadable model files (.3mf/.stl/.step/.obj) matched your search. "
                    "Try different words, or use the site buttons to browse.";
        return out;
    }

    out.ok = true;
    return out;
}

AIModelDownloadResult ai_download_model(const AIModelCandidate                              &candidate,
                                        const std::function<void(int, const std::string &)> &on_progress,
                                        const std::function<bool()>                         &is_canceled)
{
    AIModelDownloadResult out;

    if (! ai_model_url_is_supported(candidate.url)) {
        out.error = "This link isn't a supported model file (.3mf/.stl/.step/.obj).";
        return out;
    }

    AppConfig *ac = GUI::wxGetApp().app_config;
    if (! ac) {
        out.error = "Application configuration is unavailable.";
        return out;
    }

    fs::path target_dir(ac->get("download_path"));
    boost::system::error_code ec;
    if (target_dir.empty()) {
        out.error = "No download folder is configured.";
        return out;
    }
    fs::create_directories(target_dir, ec);   // best effort

    std::string filename = filename_for(candidate);

    // Avoid clobbering an existing download of the same name.
    fs::path target_path = target_dir / filename;
    if (fs::exists(target_path)) {
        const std::string stem = fs::path(filename).stem().string();
        const std::string ext  = fs::path(filename).extension().string();
        boost::uuids::uuid uuid = boost::uuids::random_generator()();
        const std::string unique = boost::uuids::to_string(uuid).substr(0, 6);
        filename    = stem + "_" + unique + ext;
        target_path = target_dir / filename;
    }

    fs::path tmp_path = target_path;
    tmp_path += ".download";

    bool        download_ok  = false;
    bool        cont         = true;
    bool        size_limit   = false;
    std::string err_msg;
    size_t      filesize     = 0;

    const int max_retries = 3;
    int       retry_count = 0;

    auto http = Http::get(candidate.url);
    while (cont && ! download_ok && retry_count < max_retries) {
        retry_count++;
        http.on_progress([&](Http::Progress progress, bool &cancel) {
                if (is_canceled && is_canceled()) { cancel = true; cont = false; return; }
                if (progress.dltotal != 0) {
                    if (filesize == 0) {
                        filesize = progress.dltotal;
                        const double megabytes = static_cast<double>(progress.dltotal) / (1024.0 * 1024.0);
                        if (megabytes > 500.0) { size_limit = true; cont = false; cancel = true; return; }
                    }
                    const int percent = static_cast<int>(progress.dlnow * 100 / progress.dltotal);
                    if (on_progress) on_progress(percent, "Downloading model...");
                } else if (on_progress) {
                    on_progress(-1, "Downloading model...");
                }
            })
            .on_error([&](std::string body, std::string error, unsigned http_status) {
                (void)body;
                BOOST_LOG_TRIVIAL(error) << "ai_download_model error: HTTP " << http_status << ", " << error;
                if (retry_count >= max_retries) {
                    err_msg = "Download failed. The site may require sign-in or block direct downloads. "
                              "Try another result, or use the site buttons to download it manually.";
                    cont = false;
                }
            })
            .on_complete([&](std::string body, unsigned /*http_status*/) {
                fs::fstream file(tmp_path, std::ios::out | std::ios::binary | std::ios::trunc);
                file.write(body.c_str(), body.size());
                file.close();
                boost::system::error_code rename_ec;
                fs::rename(tmp_path, target_path, rename_ec);
                if (rename_ec) {
                    err_msg = "Could not save the downloaded file: " + rename_ec.message();
                    return;
                }
                download_ok = true;
                cont        = false;
            })
            .perform_sync();
    }

    if (is_canceled && is_canceled()) {
        boost::system::error_code rm_ec;
        fs::remove(tmp_path, rm_ec);
        out.error = "Download canceled.";
        return out;
    }
    if (size_limit) {
        boost::system::error_code rm_ec;
        fs::remove(tmp_path, rm_ec);
        out.error = "The file is larger than the 500 MB limit.";
        return out;
    }
    if (! download_ok) {
        boost::system::error_code rm_ec;
        fs::remove(tmp_path, rm_ec);
        out.error = err_msg.empty() ? "Download failed." : err_msg;
        return out;
    }

    out.ok   = true;
    out.path = target_path.string();
    return out;
}

} // namespace Slic3r
