#include <filesystem>
#include <optional>
#include <unordered_map>
#include <iostream>
#include "Luau/Ast.h"
#include "LSP/WorkspaceFileResolver.hpp"
#include "LSP/Utils.hpp"

#include "Luau/TimeTrace.h"

Luau::ModuleName WorkspaceFileResolver::getModuleName(const Uri& name) const
{
    // Handle non-file schemes
    if (name.scheme != "file")
        return name.toString();

    auto fsPath = name.fsPath().generic_string();
    if (auto virtualPath = platform->resolveToVirtualPath(fsPath))
    {
        return *virtualPath;
    }

    return fsPath;
}

Uri WorkspaceFileResolver::getUri(const Luau::ModuleName& moduleName) const
{
    if (platform->isVirtualPath(moduleName))
    {
        if (auto filePath = platform->resolveToRealPath(moduleName))
            return Uri::file(*filePath);
    }

    // TODO: right now we map to file paths for module names, unless it's a non-file uri. Should we store uris directly instead?
    // Then this would be Uri::parse
    return Uri::file(moduleName);
}

const TextDocument* WorkspaceFileResolver::getTextDocument(const lsp::DocumentUri& uri) const
{
    auto it = managedFiles.find(uri);
    if (it != managedFiles.end())
        return &it->second;

    return nullptr;
}

const TextDocument* WorkspaceFileResolver::getTextDocumentFromModuleName(const Luau::ModuleName& name) const
{
    // managedFiles is keyed by URI. If module name is a URI that maps to a managed file, return that directly
    if (auto document = getTextDocument(Uri::parse(name)))
        return document;

    return getTextDocument(getUri(name));
}

TextDocumentPtr WorkspaceFileResolver::getOrCreateTextDocumentFromModuleName(const Luau::ModuleName& name)
{
    if (auto document = getTextDocumentFromModuleName(name))
        return TextDocumentPtr(document);

    if (auto filePath = platform->resolveToRealPath(name))
        if (auto source = readSource(name))
            return TextDocumentPtr(Uri::file(*filePath), "luau", source->source);

    return TextDocumentPtr(nullptr);
}

std::optional<Luau::SourceCode> WorkspaceFileResolver::readSource(const Luau::ModuleName& name)
{
    LUAU_TIMETRACE_SCOPE("WorkspaceFileResolver::readSource", "LSP");
    Luau::SourceCode::Type sourceType = Luau::SourceCode::Type::None;

    std::filesystem::path realFileName = name;
    if (platform->isVirtualPath(name))
    {
        auto filePath = platform->resolveToRealPath(name);
        if (!filePath)
            return std::nullopt;

        realFileName = *filePath;
        sourceType = platform->sourceCodeTypeFromPath(*filePath);
    }
    else
    {
        sourceType = platform->sourceCodeTypeFromPath(realFileName);
    }

    if (auto source = platform->readSourceCode(name, realFileName))
        return Luau::SourceCode{*source, sourceType};

    return std::nullopt;
}

std::optional<Luau::ModuleInfo> WorkspaceFileResolver::resolveModule(const Luau::ModuleInfo* context, Luau::AstExpr* node)
{
    return platform->resolveModule(context, node);
}

std::string WorkspaceFileResolver::getHumanReadableModuleName(const Luau::ModuleName& name) const
{
    if (platform->isVirtualPath(name))
    {
        if (auto realPath = platform->resolveToRealPath(name))
        {
            return realPath->fsPath().relative_path().generic_string() + " [" + name + "]";
        }
        else
        {
            return name;
        }
    }
    else
    {
        return name;
    }
}

const Luau::Config& WorkspaceFileResolver::getConfig(const Luau::ModuleName& name) const
{
    LUAU_TIMETRACE_SCOPE("WorkspaceFileResolver::getConfig", "Frontend");
    std::optional<std::filesystem::path> realPath = platform->resolveToRealPath(name);
    if (!realPath || !realPath->has_relative_path() || !realPath->has_parent_path())
        return defaultConfig;

    auto base = realPath->parent_path();
    if (isInitLuauFile(*realPath))
    {
        if (base.has_parent_path())
            base = base.parent_path();
        else
            return defaultConfig;
    }

    return readConfigRec(base);
}

std::optional<std::string> WorkspaceFileResolver::parseConfig(
    const std::filesystem::path& configPath, const std::string& contents, Luau::Config& result, bool compat)
{
    Luau::ConfigOptions::AliasOptions aliasOpts;
    aliasOpts.configLocation = configPath.parent_path().generic_string();
    aliasOpts.overwriteAliases = true;

    Luau::ConfigOptions opts;
    opts.aliasOptions = std::move(aliasOpts);
    opts.compat = compat;

    return Luau::parseConfig(contents, result, opts);
}

const Luau::Config& WorkspaceFileResolver::readConfigRec(const std::filesystem::path& path) const
{
    auto it = configCache.find(path.generic_string());
    if (it != configCache.end())
        return it->second;

    Luau::Config result = (path.has_relative_path() && path.has_parent_path()) ? readConfigRec(path.parent_path()) : defaultConfig;
    auto configPath = path / Luau::kConfigName;
    auto robloxRcPath = path / ".robloxrc";

    if (std::optional<std::string> contents = readFile(configPath))
    {
        auto configUri = Uri::file(configPath);
        std::optional<std::string> error = parseConfig(configPath, *contents, result);
        if (error)
        {
            if (client)
            {
                lsp::Diagnostic diagnostic{{{0, 0}, {0, 0}}};
                diagnostic.message = *error;
                diagnostic.severity = lsp::DiagnosticSeverity::Error;
                diagnostic.source = "Luau";
                client->publishDiagnostics({configUri, std::nullopt, {diagnostic}});
            }
            else
                // TODO: this should never be reached anymore
                std::cerr << configUri.toString() << ": " << *error;
        }
        else
        {
            if (client)
                // Clear errors presented for file
                client->publishDiagnostics({configUri, std::nullopt, {}});
        }
    }
    else if (std::optional<std::string> robloxRcContents = readFile(robloxRcPath))
    {
        // Backwards compatibility for .robloxrc files
        auto configUri = Uri::file(robloxRcPath);
        std::optional<std::string> error = parseConfig(robloxRcPath, *robloxRcContents, result, /* compat = */ true);
        if (error)
        {
            if (client)
            {
                lsp::Diagnostic diagnostic{{{0, 0}, {0, 0}}};
                diagnostic.message = *error;
                diagnostic.severity = lsp::DiagnosticSeverity::Error;
                diagnostic.source = "Luau";
                client->publishDiagnostics({configUri, std::nullopt, {diagnostic}});
            }
            else
                // TODO: this should never be reached anymore
                std::cerr << configUri.toString() << ": " << *error;
        }
        else
        {
            if (client)
                // Clear errors presented for file
                client->publishDiagnostics({configUri, std::nullopt, {}});
        }
    }

    return configCache[path.generic_string()] = result;
}

void WorkspaceFileResolver::clearConfigCache()
{
    configCache.clear();
}
