#pragma once

#include "Luau/RequireNavigator.h"

#include <optional>
#include <string>

/// Implements Luau::Require::NavigationContext for the LSP environment.
/// Follows the same design as the CLI's FileNavigationContext + VfsNavigator
/// but simplified for the LSP (absolute paths only).
class LSPNavigationContext : public Luau::Require::NavigationContext
{
public:
    using NavigateResult = Luau::Require::NavigationContext::NavigateResult;
    using ConfigStatus = Luau::Require::NavigationContext::ConfigStatus;

    explicit LSPNavigationContext(std::string requirerPath);

    // NavigationContext interface
    NavigateResult resetToRequirer() override;
    NavigateResult jumpToAlias(const std::string& path) override;
    NavigateResult toParent() override;
    NavigateResult toChild(const std::string& component) override;

    ConfigStatus getConfigStatus() const override;
    ConfigBehavior getConfigBehavior() const override;
    std::optional<std::string> getAlias(const std::string& alias) const override;
    std::optional<std::string> getConfig() const override;

    // Query methods (called after navigation)
    bool isModulePresent() const;
    std::string getResolvedPath() const;

    /// Returns the current module path (without file extension).
    /// Useful as a fallback when the module file doesn't exist on disk.
    std::string getFallbackPath() const;

private:
    NavigateResult updateRealPaths();
    std::string getConfigPath(const std::string& filename) const;
    static std::string getModulePath(std::string filePath);

    std::string requirerPath;
    std::string modulePath;
    std::string realPath;
};
