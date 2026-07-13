#include "AIProvider.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r {

// ---------------------------------------------------------------------------
// Default non-pure implementations
// ---------------------------------------------------------------------------

AIResponse AIProvider::complete(const std::string    &prompt,
                                const nlohmann::json &params) const
{
    std::vector<AIMessage> messages;
    messages.push_back({"user", prompt});
    return chat(messages, params);
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

AIProvider *AIProvider::create(const AIConfig &config)
{
    const std::string &provider = config.provider;

    if (provider.empty() || provider == "none") {
        return nullptr;
    }

    // Concrete provider implementations are added in separate tasks.
    // Stubs for known provider keys log a debug message so callers can detect
    // an intentional "provider requested but not yet compiled-in" situation.

    BOOST_LOG_TRIVIAL(debug)
        << "AIProvider::create: provider '" << provider
        << "' is not yet implemented; returning nullptr.";

    return nullptr;
}

} // namespace Slic3r
