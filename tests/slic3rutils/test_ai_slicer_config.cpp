#include <catch2/catch_all.hpp>

#include "libslic3r/AppConfig.hpp"
#include "slic3r/Utils/AIProvider.hpp"
#include "slic3r/Utils/AISlicerConfig.hpp"

using namespace Slic3r;
namespace AI = Slic3r::AISlicerConfig;

// A default-constructed AppConfig is a plain in-memory key/value store seeded
// with the editor defaults — nothing below touches the filesystem.

TEST_CASE("AISlicerConfig normalizes provider keys", "[AISlicerConfig]")
{
    CHECK(AI::normalize_provider("openai")            == AI::PROVIDER_OPENAI);
    CHECK(AI::normalize_provider("  OpenAI  ")        == AI::PROVIDER_OPENAI);
    CHECK(AI::normalize_provider("anthropic")         == AI::PROVIDER_ANTHROPIC);
    CHECK(AI::normalize_provider("compatible")        == AI::PROVIDER_COMPATIBLE);
    // Legacy spelling from earlier builds.
    CHECK(AI::normalize_provider("openai_compatible") == AI::PROVIDER_COMPATIBLE);

    // Disabled, explicitly or by omission, and anything unrecognised.
    CHECK(AI::normalize_provider("")        == AI::PROVIDER_NONE);
    CHECK(AI::normalize_provider("none")    == AI::PROVIDER_NONE);
    CHECK(AI::normalize_provider("nonsense") == AI::PROVIDER_NONE);
}

TEST_CASE("AISlicerConfig defaults a model per provider", "[AISlicerConfig]")
{
    CHECK(AI::default_model_for_provider(AI::PROVIDER_OPENAI)    == "gpt-4o-mini");
    CHECK(AI::default_model_for_provider(AI::PROVIDER_ANTHROPIC) == "claude-3-5-sonnet-latest");
    // A compatible gateway serves arbitrary models: no default to guess.
    CHECK(AI::default_model_for_provider(AI::PROVIDER_COMPATIBLE).empty());
    CHECK(AI::default_model_for_provider(AI::PROVIDER_NONE).empty());
}

TEST_CASE("AISlicerConfig getters read an empty config as disabled", "[AISlicerConfig]")
{
    AppConfig config;

    CHECK(AI::get_provider(config) == AI::PROVIDER_NONE);
    CHECK(AI::get_gateway_url(config).empty());
    CHECK(AI::get_api_key(config).empty());
    CHECK(AI::get_model(config).empty());
    CHECK_FALSE(AI::is_enabled(config));
}

TEST_CASE("AISlicerConfig setters round-trip through AppConfig", "[AISlicerConfig]")
{
    AppConfig config;

    AI::set_provider(config, "OpenAI");
    AI::set_gateway_url(config, "  https://gateway.example/v1  ");
    AI::set_api_key(config, "  sk-test-key  ");
    AI::set_model(config, "  gpt-4o  ");

    CHECK(AI::get_provider(config) == AI::PROVIDER_OPENAI);
    CHECK(AI::get_gateway_url(config) == "https://gateway.example/v1");
    CHECK(AI::get_api_key(config) == "sk-test-key");
    CHECK(AI::get_model(config) == "gpt-4o");
    CHECK(AI::is_enabled(config));

    // The values land in the section the rest of the app reads.
    CHECK(config.get(AI::SECTION, "provider") == "openai");
    CHECK(config.get(AI::SECTION, "model") == "gpt-4o");

    // Legacy provider spellings are migrated on write, not just on read.
    AI::set_provider(config, "openai_compatible");
    CHECK(config.get(AI::SECTION, "provider") == "compatible");

    // Clearing the model restores the provider default.
    AI::set_provider(config, AI::PROVIDER_ANTHROPIC);
    AI::set_model(config, "");
    CHECK(config.get(AI::SECTION, "model").empty());
    CHECK(AI::get_model(config) == "claude-3-5-sonnet-latest");
}

TEST_CASE("AISlicerConfig treats an unknown stored provider as disabled", "[AISlicerConfig]")
{
    AppConfig config;

    // Simulate a hand-edited or stale .ini rather than going through the setter.
    config.set(std::string(AI::SECTION), std::string("provider"), std::string("skynet"));
    config.set(std::string(AI::SECTION), std::string("api_key"), std::string("sk-test-key"));

    CHECK(AI::get_provider(config) == AI::PROVIDER_NONE);
    CHECK_FALSE(AI::is_enabled(config));
    CHECK(AI::get_model(config).empty());
    // The credential is still readable — only the provider selection is refused.
    CHECK(AI::get_api_key(config) == "sk-test-key");
}

TEST_CASE("AISlicerConfig provider labels are non-empty and distinct", "[AISlicerConfig]")
{
    const std::string none       = AI::provider_label(AI::PROVIDER_NONE);
    const std::string openai     = AI::provider_label(AI::PROVIDER_OPENAI);
    const std::string anthropic  = AI::provider_label(AI::PROVIDER_ANTHROPIC);
    const std::string compatible = AI::provider_label(AI::PROVIDER_COMPATIBLE);

    CHECK_FALSE(none.empty());
    CHECK_FALSE(openai.empty());
    CHECK_FALSE(anthropic.empty());
    CHECK_FALSE(compatible.empty());

    CHECK(openai != anthropic);
    CHECK(openai != compatible);
    CHECK(anthropic != compatible);
    CHECK(none != openai);

    // The legacy spelling shares the canonical provider's label.
    CHECK(AI::provider_label("openai_compatible") == compatible);
}

TEST_CASE("AIProvider::config_from_app_config reads the accessor layer", "[AISlicerConfig][AIProvider]")
{
    AppConfig config;
    AI::set_provider(config, "openai_compatible");
    AI::set_gateway_url(config, "https://gateway.example/v1");
    AI::set_api_key(config, "sk-test-key");

    AIConfig cfg = AIProvider::config_from_app_config(config);
    CHECK(cfg.provider == AI::PROVIDER_COMPATIBLE);
    CHECK(cfg.base_url == "https://gateway.example/v1");
    CHECK(cfg.api_key == "sk-test-key");
    CHECK(cfg.model.empty());   // no default for a compatible gateway

    AI::set_provider(config, AI::PROVIDER_OPENAI);
    CHECK(AIProvider::config_from_app_config(config).model == "gpt-4o-mini");
}
