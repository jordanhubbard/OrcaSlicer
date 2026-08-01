#include "AISlicerConfig.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/I18N.hpp"

namespace Slic3r {
namespace AISlicerConfig {

const char *SECTION = "ai_slicer";

const char *PROVIDER_NONE       = "";
const char *PROVIDER_OPENAI     = "openai";
const char *PROVIDER_ANTHROPIC  = "anthropic";
const char *PROVIDER_COMPATIBLE = "compatible";

namespace {

// Keys inside SECTION. Kept private so every read/write goes through the
// accessors below.
const char *KEY_PROVIDER    = "provider";
const char *KEY_GATEWAY_URL = "gateway_url";
const char *KEY_API_KEY     = "api_key";
const char *KEY_MODEL       = "model";

std::string trimmed(const std::string &s)
{
    std::string out = s;
    boost::trim(out);
    return out;
}

} // namespace

std::string normalize_provider(const std::string &raw)
{
    std::string key = trimmed(raw);
    boost::to_lower(key);

    if (key == PROVIDER_OPENAI)     return PROVIDER_OPENAI;
    if (key == PROVIDER_ANTHROPIC)  return PROVIDER_ANTHROPIC;
    // "openai_compatible" is the spelling shipped by earlier builds.
    if (key == PROVIDER_COMPATIBLE || key == "openai_compatible")
        return PROVIDER_COMPATIBLE;

    // "none" is the explicit disabled marker; anything else is a stale or
    // hand-edited value we refuse to guess about. Both read as disabled.
    if (! key.empty() && key != "none")
        BOOST_LOG_TRIVIAL(warning)
            << "AISlicerConfig: unknown provider '" << key << "'; treating AI Slicer as disabled.";
    return PROVIDER_NONE;
}

std::string provider_label(const std::string &provider)
{
    const std::string key = normalize_provider(provider);
    if (key == PROVIDER_OPENAI)     return _u8L("OpenAI");
    if (key == PROVIDER_ANTHROPIC)  return _u8L("Anthropic");
    if (key == PROVIDER_COMPATIBLE) return _u8L("OpenAI-compatible gateway");
    return _u8L("Disabled");
}

std::string default_model_for_provider(const std::string &provider)
{
    const std::string key = normalize_provider(provider);
    if (key == PROVIDER_OPENAI)    return "gpt-4o-mini";
    if (key == PROVIDER_ANTHROPIC) return "claude-3-5-sonnet-latest";
    // A compatible gateway serves whatever models its operator loaded, so there
    // is nothing sensible to guess; the back-end falls back at request time.
    return std::string();
}

// --- Getters ---------------------------------------------------------------

std::string get_provider(const AppConfig &config)
{
    return normalize_provider(config.get(SECTION, KEY_PROVIDER));
}

std::string get_gateway_url(const AppConfig &config)
{
    return trimmed(config.get(SECTION, KEY_GATEWAY_URL));
}

std::string get_api_key(const AppConfig &config)
{
    // SECURITY: AppConfig has no encrypted fields, so this key was read back
    // verbatim from the cleartext OrcaSlicer .ini. Pass it straight to the
    // provider request; do not log it, echo it in an error message, or copy it
    // into a crash report.
    return trimmed(config.get(SECTION, KEY_API_KEY));
}

std::string get_model(const AppConfig &config)
{
    const std::string model = trimmed(config.get(SECTION, KEY_MODEL));
    return model.empty() ? default_model_for_provider(get_provider(config)) : model;
}

bool is_enabled(const AppConfig &config)
{
    return get_provider(config) != PROVIDER_NONE;
}

// --- Setters ---------------------------------------------------------------

void set_provider(AppConfig &config, const std::string &provider)
{
    config.set(SECTION, KEY_PROVIDER, normalize_provider(provider));
}

void set_gateway_url(AppConfig &config, const std::string &url)
{
    config.set(SECTION, KEY_GATEWAY_URL, trimmed(url));
}

void set_api_key(AppConfig &config, const std::string &api_key)
{
    // SECURITY: this is written in cleartext to the OrcaSlicer .ini — AppConfig
    // stores plain key/value pairs and the file is not encrypted. Anyone with
    // read access to the user's config directory can recover the credential.
    // Log that a key was stored, never the key itself.
    const std::string key = trimmed(api_key);
    BOOST_LOG_TRIVIAL(debug)
        << "AISlicerConfig: " << (key.empty() ? "clearing" : "storing")
        << " the AI Slicer API key (stored in cleartext in the application .ini).";
    config.set(SECTION, KEY_API_KEY, key);
}

void set_model(AppConfig &config, const std::string &model)
{
    config.set(SECTION, KEY_MODEL, trimmed(model));
}

} // namespace AISlicerConfig
} // namespace Slic3r
