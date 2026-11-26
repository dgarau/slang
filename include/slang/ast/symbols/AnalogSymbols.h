//------------------------------------------------------------------------------
//! @file AnalogSymbols.h
//! @brief SVMake VAMS P0.1: Verilog-AMS (Accellera VAMS-2023) symbols
//
// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include <optional>

#include "slang/ast/Symbol.h"
#include "slang/syntax/SyntaxFwd.h"
#include "slang/util/SmallVector.h"

namespace slang::ast {

class ASTContext;
class ASTSerializer;
class Expression;
class NetSymbol;
class Scope;

/// True if @a scope belongs to a definition parsed as Verilog-AMS source text
/// (DefinitionSymbol::isVams). Compilation-unit scopes are never VAMS scopes.
SLANG_EXPORT bool isVamsScope(const Scope& scope);

/// A Verilog-AMS nature (VAMS-2023 3.6.1). Natures are owned by the Compilation
/// (Compilation::getNatures), not by any scope's name map; their parent scope is the
/// compilation unit that declared them, used only to bind attribute expressions.
class SLANG_EXPORT NatureSymbol final : public Symbol {
public:
    /// A user-defined attribute (3.6.1.3).
    struct UserAttribute {
        std::string_view name;
        const syntax::ExpressionSyntax* value = nullptr;
    };

    NatureSymbol(std::string_view name, SourceLocation loc) :
        Symbol(SymbolKind::Nature, name, loc) {}

    /// The parent nature, or null for a base nature.
    const NatureSymbol* getParent() const;

    /// The base nature: this nature for a base nature, else the parent's base.
    const NatureSymbol& getBase() const;

    /// The access function name (inherited from the parent).
    std::string_view getAccessName() const;

    /// The units string (inherited from the parent).
    std::string_view getUnits() const;

    /// The absolute tolerance (inherited from the parent when not set).
    std::optional<double> getAbstol() const;

    /// The time-derivative and time-integral natures (default: the nature itself).
    const NatureSymbol* getDdtNature() const;
    const NatureSymbol* getIdtNature() const;

    std::span<const UserAttribute> getUserAttributes() const;

    /// Resolves everything and issues this nature's diagnostics.
    void check() const;

    void serializeTo(ASTSerializer& serializer) const;

    static bool isKind(SymbolKind kind) { return kind == SymbolKind::Nature; }

private:
    void resolve() const;

    mutable bool resolved = false;
    mutable bool resolving = false;
    mutable const NatureSymbol* parent = nullptr;
    mutable std::string_view accessName;
    mutable std::string_view units;
    mutable std::optional<double> abstol;
    mutable std::string_view ddtName;
    mutable std::string_view idtName;
    mutable std::span<const UserAttribute> userAttributes;
};

/// A Verilog-AMS discipline (VAMS-2023 3.6.2). Owned by the Compilation, like natures.
class SLANG_EXPORT DisciplineSymbol final : public Symbol {
public:
    enum class Domain { Unspecified, Discrete, Continuous };

    DisciplineSymbol(std::string_view name, SourceLocation loc) :
        Symbol(SymbolKind::Discipline, name, loc) {}

    const NatureSymbol* getPotential() const;
    const NatureSymbol* getFlow() const;

    /// The declared domain binding.
    Domain getDomain() const;

    /// The domain in effect: the declared one, else continuous when a nature is bound.
    Domain getEffectiveDomain() const;

    bool isContinuous() const { return getEffectiveDomain() == Domain::Continuous; }

    /// No domain binding and no natures (3.6.2.3).
    bool isDomainless() const;

    /// No natures bound (3.6.2.3).
    bool isNatureless() const;

    /// Discipline compatibility, VAMS-2023 3.11.1.
    bool isCompatibleWith(const DisciplineSymbol& other) const;

    /// Nature compatibility, VAMS-2023 3.11.1 (a null nature is a non-existent binding).
    static bool areCompatible(const NatureSymbol* a, const NatureSymbol* b);

    void check() const;

    void serializeTo(ASTSerializer& serializer) const;

    static bool isKind(SymbolKind kind) { return kind == SymbolKind::Discipline; }

private:
    void resolve() const;

    mutable bool resolved = false;
    mutable const NatureSymbol* potential = nullptr;
    mutable const NatureSymbol* flow = nullptr;
    mutable Domain domain = Domain::Unspecified;
};

/// One terminal of a branch or of a branch access function: a net with a continuous
/// discipline, optionally with a constant bit index.
struct SLANG_EXPORT AnalogTerminal {
    const Expression* expr = nullptr;
    const NetSymbol* net = nullptr;
    std::optional<int32_t> index;

    bool sameAs(const AnalogTerminal& other) const {
        return net && net == other.net && index == other.index;
    }

    /// Binds @a syntax as a terminal and checks it; returns an empty terminal (net == null)
    /// after issuing a diagnostic when it is not one.
    static AnalogTerminal bind(const syntax::ExpressionSyntax& syntax, const ASTContext& context);
};

/// A named Verilog-AMS branch (VAMS-2023 3.12): `branch (a, b) name;`.
class SLANG_EXPORT BranchSymbol final : public Symbol {
public:
    BranchSymbol(std::string_view name, SourceLocation loc) :
        Symbol(SymbolKind::Branch, name, loc) {}

    /// The positive terminal.
    const AnalogTerminal& getPositive() const;

    /// The negative terminal, or null when the branch goes to implicit ground.
    const AnalogTerminal* getNegative() const;

    /// The branch discipline (from its terminals), or null on error.
    const DisciplineSymbol* getDiscipline() const;

    void serializeTo(ASTSerializer& serializer) const;

    static void fromSyntax(const Scope& scope, const syntax::BranchDeclarationSyntax& syntax,
                           SmallVectorBase<const BranchSymbol*>& results);

    static bool isKind(SymbolKind kind) { return kind == SymbolKind::Branch; }

private:
    void resolve() const;

    mutable bool resolved = false;
    mutable AnalogTerminal positive;
    mutable std::optional<AnalogTerminal> negative;
    mutable const DisciplineSymbol* discipline = nullptr;
};

} // namespace slang::ast
