#include "Platform/LSPNavigationContext.hpp"

#include "LuauFileUtils.hpp"
#include "Luau/Common.h"
#include "Luau/Config.h"
#include "Luau/LuauConfig.h"

#include <array>
#include <string>
#include <string_view>

static const std::array<std::string_view, 2> kSuffixes = {".luau", ".lua"};
static const std::array<std::string_view, 2> kInitSuffixes = {"/init.luau", "/init.lua"};

static bool hasSuffix(std::string_view str, std::string_view suffix)
{
    return str.size() >= suffix.size() && str.substr(str.size() - suffix.size()) == suffix;
}

struct ResolvedRealPath
{
    LSPNavigationContext::NavigateResult status;
    std::string realPath;
};

static ResolvedRealPath getRealPath(const std::string& modulePath)
{
    bool found = false;
    std::string suffix;

    size_t lastSlash = modulePath.find_last_of('/');
    LUAU_ASSERT(lastSlash != std::string::npos);
    std::string lastComponent = modulePath.substr(lastSlash + 1);

    if (lastComponent != "init")
    {
        for (std::string_view potentialSuffix : kSuffixes)
        {
            if (Luau::FileUtils::isFile(modulePath + std::string(potentialSuffix)))
            {
                if (found)
                    return {LSPNavigationContext::NavigateResult::Ambiguous, {}};

                suffix = potentialSuffix;
                found = true;
            }
        }
    }
    if (Luau::FileUtils::isDirectory(modulePath))
    {
        if (found)
            return {LSPNavigationContext::NavigateResult::Ambiguous, {}};

        for (std::string_view potentialSuffix : kInitSuffixes)
        {
            if (Luau::FileUtils::isFile(modulePath + std::string(potentialSuffix)))
            {
                if (found)
                    return {LSPNavigationContext::NavigateResult::Ambiguous, {}};

                suffix = potentialSuffix;
                found = true;
            }
        }

        // Directory exists (with or without init file) — treat as found so navigation can proceed.
        found = true;
    }

    if (!found)
        return {LSPNavigationContext::NavigateResult::NotFound, {}};

    return {LSPNavigationContext::NavigateResult::Success, modulePath + suffix};
}

std::string LSPNavigationContext::getModulePath(std::string filePath)
{
    for (char& c : filePath)
    {
        if (c == '\\')
            c = '/';
    }

    std::string_view pathView = filePath;

    for (std::string_view suffix : kInitSuffixes)
    {
        if (hasSuffix(pathView, suffix))
        {
            pathView.remove_suffix(suffix.size());
            return std::string(pathView);
        }
    }
    for (std::string_view suffix : kSuffixes)
    {
        if (hasSuffix(pathView, suffix))
        {
            pathView.remove_suffix(suffix.size());
            return std::string(pathView);
        }
    }

    return std::string(pathView);
}

LSPNavigationContext::LSPNavigationContext(std::string requirerPath)
    : requirerPath(std::move(requirerPath))
{
}

LSPNavigationContext::NavigateResult LSPNavigationContext::updateRealPaths()
{
    ResolvedRealPath result = getRealPath(modulePath);
    if (result.status == NavigateResult::Ambiguous)
        return NavigateResult::Ambiguous;

    realPath = std::move(result.realPath);
    return NavigateResult::Success;
}

LSPNavigationContext::NavigateResult LSPNavigationContext::resetToRequirer()
{
    std::string normalizedPath = Luau::FileUtils::normalizePath(requirerPath);

    LUAU_ASSERT(Luau::FileUtils::isAbsolutePath(normalizedPath));

    modulePath = getModulePath(normalizedPath);
    return updateRealPaths();
}

LSPNavigationContext::NavigateResult LSPNavigationContext::jumpToAlias(const std::string& path)
{
    if (!Luau::FileUtils::isAbsolutePath(path))
        return NavigateResult::NotFound;

    modulePath = getModulePath(Luau::FileUtils::normalizePath(path));
    return updateRealPaths();
}

LSPNavigationContext::NavigateResult LSPNavigationContext::toParent()
{
    if (modulePath == "/")
        return NavigateResult::NotFound;

    size_t numSlashes = 0;
    for (char c : modulePath)
    {
        if (c == '/')
            numSlashes++;
    }
    LUAU_ASSERT(numSlashes > 0);

    if (numSlashes == 1)
        return NavigateResult::NotFound;

    modulePath = Luau::FileUtils::normalizePath(modulePath + "/..");

    // There is no ambiguity when navigating up in a tree (matches VfsNavigator behavior).
    NavigateResult status = updateRealPaths();
    return status == NavigateResult::Ambiguous ? NavigateResult::Success : status;
}

LSPNavigationContext::NavigateResult LSPNavigationContext::toChild(const std::string& component)
{
    if (component == ".config")
        return NavigateResult::NotFound;

    modulePath = Luau::FileUtils::normalizePath(modulePath + "/" + component);
    return updateRealPaths();
}

std::string LSPNavigationContext::getConfigPath(const std::string& filename) const
{
    std::string_view directory = realPath;

    for (std::string_view suffix : kInitSuffixes)
    {
        if (hasSuffix(directory, suffix))
        {
            directory.remove_suffix(suffix.size());
            return std::string(directory) + '/' + filename;
        }
    }
    for (std::string_view suffix : kSuffixes)
    {
        if (hasSuffix(directory, suffix))
        {
            directory.remove_suffix(suffix.size());
            return std::string(directory) + '/' + filename;
        }
    }

    return std::string(directory) + '/' + filename;
}

LSPNavigationContext::ConfigStatus LSPNavigationContext::getConfigStatus() const
{
    bool luaurcExists = Luau::FileUtils::isFile(getConfigPath(Luau::kConfigName));
    bool luauConfigExists = Luau::FileUtils::isFile(getConfigPath(Luau::kLuauConfigName));

    if (luaurcExists && luauConfigExists)
        return ConfigStatus::Ambiguous;
    else if (luauConfigExists)
        return ConfigStatus::PresentLuau;
    else if (luaurcExists)
        return ConfigStatus::PresentJson;
    else
        return ConfigStatus::Absent;
}

LSPNavigationContext::ConfigBehavior LSPNavigationContext::getConfigBehavior() const
{
    return ConfigBehavior::GetConfig;
}

std::optional<std::string> LSPNavigationContext::getAlias(const std::string& alias) const
{
    return std::nullopt;
}

std::optional<std::string> LSPNavigationContext::getConfig() const
{
    ConfigStatus status = getConfigStatus();
    LUAU_ASSERT(status == ConfigStatus::PresentJson || status == ConfigStatus::PresentLuau);

    if (status == ConfigStatus::PresentJson)
        return Luau::FileUtils::readFile(getConfigPath(Luau::kConfigName));
    else if (status == ConfigStatus::PresentLuau)
        return Luau::FileUtils::readFile(getConfigPath(Luau::kLuauConfigName));

    LUAU_UNREACHABLE();
}

bool LSPNavigationContext::isModulePresent() const
{
    return Luau::FileUtils::isFile(realPath);
}

std::string LSPNavigationContext::getResolvedPath() const
{
    return realPath;
}

std::string LSPNavigationContext::getFallbackPath() const
{
    if (modulePath.empty())
        return {};
    return modulePath + ".luau";
}
