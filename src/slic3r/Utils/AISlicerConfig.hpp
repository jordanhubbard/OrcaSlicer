#pragma once

#include <string>

namespace Slic3r {

class AppConfig;   // libslic3r

/**
 * Typed accessors for the "[ai_slicer]" section of AppConfig.
 *
 * Every AI Slicer setting lives in one free-form AppConfig section and is
 * reached through AppConfig::get(section, key) / AppConfig::set(section, key,
 * value). Spelling those raw section/key pairs at each call site is how keys
 * drift apart, so the whole section is funnelled through the getters and
 * setters below: they own the key names, the normalisation of legacy values,
 * and the defaults.
 *
 * This is the configuration layer only — it never talks to a provider. Use
 * AIProvider::config_from_app_config() to turn these values into an AIConfig
 * and AIProvider::create() to instantiate a back-end.
 *
 * SECURITY: the API key is stored in cleartext in the OrcaSlicer .ini, exactly
 * like every other AppConfig value — the .ini has no encrypted fields. Anyone
 * who can read the user's config directory can read the key. See the note on
 * get_api_key()/set_api_key() before adding further credential settings.
 */
namespace AISlicerConfig {

/// AppConfig section holding every AI Slicer setting.
extern const char *SECTION;

// --- Provider keys ---------------------------------------------------------
// The values stored in "ai_slicer/provider". PROVIDER_NONE (the empty string)
// means "AI Slicer disabled" and is the default.

extern const char *PROVIDER_NONE;         ///< ""           — disabled
extern const char *PROVIDER_OPENAI;       ///< "openai"
extern const char *PROVIDER_ANTHROPIC;    ///< "anthropic"
extern const char *PROVIDER_COMPATIBLE;   ///< "compatible" — any OpenAI-compatible gateway

/**
 * Canonicalise a raw provider string.
 *
 * Trims surrounding whitespace, lower-cases, maps the legacy spellings
 * "openai_compatible" and "none" onto "compatible" and "" respectively, and
 * returns PROVIDER_NONE for anything unrecognised so that a corrupt or
 * hand-edited .ini reads as "disabled" rather than as a live provider.
 */
std::string normalize_provider(const std::string &raw);

/// Localised, user-facing display name for a provider key ("" -> "Disabled").
std::string provider_label(const std::string &provider);

/// Model id used when "ai_slicer/model" is unset, or "" when the provider has
/// no meaningful default (the back-end picks one at request time).
std::string default_model_for_provider(const std::string &provider);

// --- Getters ---------------------------------------------------------------

/// Configured provider key, normalised; PROVIDER_NONE when unset or unknown.
std::string get_provider(const AppConfig &config);

/// Base URL override for the provider endpoint; "" when unset.
/// Required for PROVIDER_COMPATIBLE, optional for the others.
std::string get_gateway_url(const AppConfig &config);

/// Provider credential; "" when unset.
/// SECURITY: returned (and stored) in cleartext — see the note on the
/// namespace above. Never write this value to a log or a crash report.
std::string get_api_key(const AppConfig &config);

/// Configured model id, falling back to default_model_for_provider() for the
/// currently configured provider. May be "" when neither is set.
std::string get_model(const AppConfig &config);

/// true when a provider is configured, i.e. get_provider() != PROVIDER_NONE.
/// Does not check that the credentials are usable — that needs a live request.
bool is_enabled(const AppConfig &config);

// --- Setters ---------------------------------------------------------------
// All setters normalise/trim their input and mark AppConfig dirty through
// AppConfig::set(), so the caller only has to persist as usual.

/// Store the provider key, normalised through normalize_provider().
void set_provider(AppConfig &config, const std::string &provider);

/// Store the gateway base URL (trimmed).
void set_gateway_url(AppConfig &config, const std::string &url);

/// Store the provider credential.
/// SECURITY: written in cleartext to the .ini — see the namespace note above.
void set_api_key(AppConfig &config, const std::string &api_key);

/// Store the model id (trimmed). Storing "" restores the provider default
/// reported by get_model().
void set_model(AppConfig &config, const std::string &model);

} // namespace AISlicerConfig

} // namespace Slic3r
