#ifndef slic3r_AIModelSearch_hpp_
#define slic3r_AIModelSearch_hpp_

#include <functional>
#include <string>
#include <vector>

namespace Slic3r {

// ---------------------------------------------------------------------------
// AIModelSearch
//
// Turns a natural-language query into a small list of candidate 3D-model
// downloads and imports the chosen one onto the plate.
//
// Discovery: the configured AIProvider is asked (via a forced `suggest_models`
// tool/function call) to return known model-repository pages that match the
// query. Each candidate carries a human title, a direct file URL, and the
// source site it came from. We only surface links to the public model
// repositories the user already uses; we never scrape or bypass a site's terms
// of service or sign-in.
//
// Import: the chosen candidate's file is downloaded to
// app_config->get("download_path") using Slic3r::Http (mirroring
// Plater::import_model_id) and then handed to Plater::load_files(), which
// already knows how to read 3MF / STL / STEP / OBJ.
// ---------------------------------------------------------------------------

/// A single search hit the user can choose to import.
struct AIModelCandidate
{
    std::string title;   ///< Human-readable model name.
    std::string url;      ///< Direct download URL for a model file.
    std::string source;  ///< Originating site (e.g. "Printables", "Thingiverse").
};

/// Outcome of a search request.
struct AIModelSearchResult
{
    bool                          ok { false };
    std::string                   error;       ///< Human-readable failure (when !ok).
    std::vector<AIModelCandidate> candidates;  ///< Matches (possibly empty).
};

/// Outcome of a download request.
struct AIModelDownloadResult
{
    bool        ok { false };
    std::string error;   ///< Human-readable failure (when !ok).
    std::string path;    ///< Absolute path of the downloaded file (when ok).
};

/**
 * File extensions Orca can import through Plater::load_files(). A candidate URL
 * whose path does not end in one of these is rejected before download.
 */
bool ai_model_url_is_supported(const std::string &url);

/**
 * Ask the configured AIProvider for candidate model downloads matching `query`.
 *
 * Blocking network call — run it off the GUI thread. Returns ok==false with a
 * populated `error` when no provider is configured, the request fails, or the
 * model returns nothing usable.
 */
AIModelSearchResult ai_search_models(const std::string &query);

/**
 * Download `candidate` into app_config->get("download_path"), reporting
 * progress through `on_progress(percent, message)` (percent may be -1 when the
 * total size is unknown). `is_canceled` is polled to allow abort.
 *
 * Blocking network call — run it off the GUI thread. Mirrors the download half
 * of Plater::import_model_id (retries, size guard, temp-file rename).
 */
AIModelDownloadResult ai_download_model(const AIModelCandidate                       &candidate,
                                        const std::function<void(int, const std::string &)> &on_progress,
                                        const std::function<bool()>                          &is_canceled);

} // namespace Slic3r

#endif // slic3r_AIModelSearch_hpp_
