//------------------------------------------------------------------------------
// AnalogSymbols.cpp
// SVMake VAMS P0.1: Verilog-AMS (Accellera VAMS-2023) symbols
//
// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#include "slang/ast/symbols/AnalogSymbols.h"

#include "slang/ast/ASTContext.h"
#include "slang/ast/ASTSerializer.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/Expression.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/Type.h"
#include "slang/diagnostics/LookupDiags.h"
#include "slang/diagnostics/VamsDiags.h"
#include "slang/syntax/AllSyntax.h"

namespace slang::ast {

using namespace syntax;
using namespace parsing;

bool isVamsScope(const Scope& scope) {
    const Scope* current = &scope;
    while (current) {
        auto& sym = current->asSymbol();
        switch (sym.kind) {
            case SymbolKind::InstanceBody:
                return sym.as<InstanceBodySymbol>().getDefinition().isVams;
            case SymbolKind::CompilationUnit:
            case SymbolKind::Root:
            case SymbolKind::Package:
                return false;
            default:
                break;
        }
        current = sym.getParentScope();
    }
    return false;
}

// ----- NatureSymbol -----

static std::string_view nameValue(const ExpressionSyntax& expr) {
    if (expr.kind == SyntaxKind::IdentifierName)
        return expr.as<IdentifierNameSyntax>().identifier.valueText();
    return {};
}

void NatureSymbol::resolve() const {
    if (resolved || resolving)
        return;
    resolving = true;

    auto scope = getParentScope();
    auto syntax = getSyntax();
    SLANG_ASSERT(scope && syntax);
    auto& comp = scope->getCompilation();
    auto& decl = syntax->as<NatureDeclarationSyntax>();
    ASTContext context(*scope, LookupLocation::max);

    // parent_nature ::= nature_identifier | discipline_identifier . potential_or_flow
    if (auto pc = decl.parent) {
        auto parentName = pc->name.valueText();
        if (pc->dot) {
            auto disc = comp.getDiscipline(parentName);
            if (!disc) {
                if (!parentName.empty())
                    scope->addDiag(diag::UnknownDiscipline, pc->name.range()) << parentName;
            }
            else {
                parent = pc->potentialOrFlow.kind == TokenKind::FlowKeyword ? disc->getFlow()
                                                                            : disc->getPotential();
            }
        }
        else if (!parentName.empty()) {
            parent = comp.getNature(parentName);
            if (!parent)
                scope->addDiag(diag::UnknownNature, pc->name.range()) << parentName;
        }

        if (parent) {
            parent->resolve();
            if (parent == this || parent->resolving) {
                // A cycle in the parent chain.
                scope->addDiag(diag::NatureCycle, pc->name.range()) << name;
                parent = nullptr;
            }
        }
    }

    bool hasAccess = false, hasUnits = false, hasAbsTol = false;
    SmallVector<UserAttribute> users;
    for (auto item : decl.items) {
        auto attrName = item->name.valueText();
        auto& value = *item->value;
        switch (item->name.kind) {
            case TokenKind::AccessKeyword: {
                // 3.6.1.2: an identifier, by name; a derived nature cannot change it.
                auto name = nameValue(value);
                if (name.empty()) {
                    scope->addDiag(diag::NatureAttributeNotName, value.sourceRange()) << attrName;
                }
                else if (parent) {
                    scope->addDiag(diag::NatureAttributeChanged, item->name.range()) << attrName;
                }
                else {
                    accessName = name;
                }
                hasAccess = true;
                break;
            }
            case TokenKind::UnitsKeyword: {
                // 3.6.1.2: a string; a derived nature cannot define or change it.
                if (parent) {
                    scope->addDiag(diag::NatureAttributeChanged, item->name.range()) << attrName;
                    break;
                }
                auto& expr = Expression::bind(value, context);
                auto cv = context.eval(expr);
                if (expr.bad()) {
                }
                else if (!expr.isImplicitString() && !cv.isString()) {
                    scope->addDiag(diag::NatureUnitsNotString, value.sourceRange());
                }
                else if (cv) {
                    auto str = cv.isString() ? cv.str() : cv.convertToStr().str();
                    auto mem = (char*)comp.allocate(str.size(), 1);
                    std::ranges::copy(str, mem);
                    units = std::string_view(mem, str.size());
                }
                hasUnits = true;
                break;
            }
            case TokenKind::AbsTolKeyword: {
                auto& expr = Expression::bind(value, context);
                if (expr.bad()) {
                }
                else if (expr.kind == ExpressionKind::StringLiteral || !expr.type->isNumeric()) {
                    scope->addDiag(diag::NatureAbsTolNotNumeric, value.sourceRange());
                }
                else if (auto cv = context.eval(expr)) {
                    abstol = cv.convertToReal().real();
                }
                hasAbsTol = true;
                break;
            }
            case TokenKind::DdtNatureKeyword:
            case TokenKind::IdtNatureKeyword: {
                auto name = nameValue(value);
                if (name.empty()) {
                    scope->addDiag(diag::NatureAttributeNotName, value.sourceRange()) << attrName;
                    break;
                }
                if (!comp.getNature(name))
                    scope->addDiag(diag::UnknownNature, value.sourceRange()) << name;
                if (item->name.kind == TokenKind::DdtNatureKeyword)
                    ddtName = name;
                else
                    idtName = name;
                break;
            }
            default:
                users.push_back({attrName, &value});
                break;
        }
    }
    userAttributes = users.copy(comp);

    if (parent) {
        // Inherited attributes (3.6.1.1, 3.6.1.2).
        accessName = parent->getAccessName();
        units = parent->getUnits();
        if (!hasAbsTol)
            abstol = parent->getAbsTol();
        if (ddtName.empty() && parent->getDdtNature() != parent)
            ddtName = parent->getDdtNature() ? parent->getDdtNature()->name : ""sv;
        if (idtName.empty() && parent->getIdtNature() != parent)
            idtName = parent->getIdtNature() ? parent->getIdtNature()->name : ""sv;
    }
    else if (!decl.parent) {
        // 3.6.1.2: abstol, access and units are required for all base natures.
        auto missing = [&](std::string_view attr) {
            scope->addDiag(diag::NatureMissingAttribute, location) << name << attr;
        };
        if (!hasAbsTol)
            missing("abstol"sv);
        if (!hasAccess)
            missing("access"sv);
        if (!hasUnits)
            missing("units"sv);
    }

    resolving = false;
    resolved = true;
}

const NatureSymbol* NatureSymbol::getParent() const {
    resolve();
    return parent;
}

const NatureSymbol& NatureSymbol::getBase() const {
    auto current = this;
    for (int depth = 0; depth < 64; depth++) {
        auto p = current->getParent();
        if (!p)
            break;
        current = p;
    }
    return *current;
}

std::string_view NatureSymbol::getAccessName() const {
    resolve();
    return accessName;
}

std::string_view NatureSymbol::getUnits() const {
    resolve();
    return units;
}

std::optional<double> NatureSymbol::getAbsTol() const {
    resolve();
    return abstol;
}

const NatureSymbol* NatureSymbol::getDdtNature() const {
    resolve();
    if (ddtName.empty())
        return this;
    return getParentScope()->getCompilation().getNature(ddtName);
}

const NatureSymbol* NatureSymbol::getIdtNature() const {
    resolve();
    if (idtName.empty())
        return this;
    return getParentScope()->getCompilation().getNature(idtName);
}

std::span<const NatureSymbol::UserAttribute> NatureSymbol::getUserAttributes() const {
    resolve();
    return userAttributes;
}

void NatureSymbol::check() const {
    resolve();
}

void NatureSymbol::serializeTo(ASTSerializer& serializer) const {
    if (auto p = getParent())
        serializer.writeLink("parent", *p);
    serializer.write("access", getAccessName());
    serializer.write("units", getUnits());
    if (auto a = getAbsTol())
        serializer.write("abstol", *a);
    if (auto d = getDdtNature(); d && d != this)
        serializer.writeLink("ddtNature", *d);
    if (auto i = getIdtNature(); i && i != this)
        serializer.writeLink("idtNature", *i);
}

// ----- DisciplineSymbol -----

void DisciplineSymbol::resolve() const {
    if (resolved)
        return;
    resolved = true;

    auto scope = getParentScope();
    auto syntax = getSyntax();
    SLANG_ASSERT(scope && syntax);
    auto& comp = scope->getCompilation();
    auto& decl = syntax->as<DisciplineDeclarationSyntax>();

    bool sawPotential = false, sawFlow = false, sawDomain = false;
    for (auto item : decl.items) {
        switch (item->kind) {
            case SyntaxKind::DisciplineNatureBinding: {
                auto& nb = item->as<DisciplineNatureBindingSyntax>();
                bool isFlow = nb.potentialOrFlow.kind == TokenKind::FlowKeyword;
                bool& seen = isFlow ? sawFlow : sawPotential;
                if (seen) {
                    scope->addDiag(diag::DisciplineDuplicateBinding, nb.potentialOrFlow.range())
                        << name << (isFlow ? "flow nature"sv : "potential nature"sv);
                    break;
                }
                seen = true;

                auto natureName = nb.nature.valueText();
                auto nature = comp.getNature(natureName);
                if (!nature) {
                    if (!natureName.empty())
                        scope->addDiag(diag::UnknownNature, nb.nature.range()) << natureName;
                }
                else if (isFlow) {
                    flow = nature;
                }
                else {
                    potential = nature;
                }
                break;
            }
            case SyntaxKind::DisciplineDomainBinding: {
                auto& db = item->as<DisciplineDomainBindingSyntax>();
                if (sawDomain) {
                    scope->addDiag(diag::DisciplineDuplicateBinding, db.domain.range())
                        << name << "domain"sv;
                    break;
                }
                sawDomain = true;
                domain = db.value.kind == TokenKind::DiscreteKeyword ? Domain::Discrete
                                                                     : Domain::Continuous;
                break;
            }
            default:
                // Attribute overrides (3.6.2.5) are stored in the syntax; their semantics
                // are P1.
                break;
        }
    }

    if (potential && flow && !potential->getAccessName().empty() &&
        potential->getAccessName() == flow->getAccessName()) {
        scope->addDiag(diag::DisciplineAccessConflict, location)
            << name << potential->getAccessName();
    }
}

const NatureSymbol* DisciplineSymbol::getPotential() const {
    resolve();
    return potential;
}

const NatureSymbol* DisciplineSymbol::getFlow() const {
    resolve();
    return flow;
}

DisciplineSymbol::Domain DisciplineSymbol::getDomain() const {
    resolve();
    return domain;
}

DisciplineSymbol::Domain DisciplineSymbol::getEffectiveDomain() const {
    resolve();
    if (domain != Domain::Unspecified)
        return domain;
    // 3.6.2.2: the default domain is continuous for a discipline that binds natures.
    return (potential || flow) ? Domain::Continuous : Domain::Unspecified;
}

bool DisciplineSymbol::isNatureless() const {
    resolve();
    return !potential && !flow;
}

bool DisciplineSymbol::isDomainless() const {
    resolve();
    return domain == Domain::Unspecified && isNatureless();
}

bool DisciplineSymbol::areCompatible(const NatureSymbol* a, const NatureSymbol* b) {
    // Self rule, non-existent binding rule.
    if (a == b || !a || !b)
        return true;

    // Base nature rule and derived nature rule: compatible when they share a base.
    if (&a->getBase() == &b->getBase())
        return true;

    // Units value rule.
    auto ua = a->getUnits(), ub = b->getUnits();
    return !ua.empty() && ua == ub;
}

bool DisciplineSymbol::isCompatibleWith(const DisciplineSymbol& other) const {
    // Self rule.
    if (this == &other)
        return true;

    // Domainless discipline rule.
    if (isDomainless() || other.isDomainless())
        return true;

    // Domain incompatibility rule.
    if (getEffectiveDomain() != other.getEffectiveDomain())
        return false;

    // Natureless discipline rule (same domain established above).
    if (isNatureless() || other.isNatureless())
        return true;

    // Potential and flow incompatibility rules.
    return areCompatible(getPotential(), other.getPotential()) &&
           areCompatible(getFlow(), other.getFlow());
}

void DisciplineSymbol::check() const {
    resolve();
}

void DisciplineSymbol::serializeTo(ASTSerializer& serializer) const {
    if (auto p = getPotential())
        serializer.writeLink("potential", *p);
    if (auto f = getFlow())
        serializer.writeLink("flow", *f);
    switch (getDomain()) {
        case Domain::Discrete:
            serializer.write("domain", "discrete"sv);
            break;
        case Domain::Continuous:
            serializer.write("domain", "continuous"sv);
            break;
        case Domain::Unspecified:
            break;
    }
}

// ----- AnalogTerminal -----

AnalogTerminal AnalogTerminal::bind(const ExpressionSyntax& syntax, const ASTContext& context) {
    AnalogTerminal result;
    auto& expr = Expression::bind(syntax, context);
    result.expr = &expr;
    if (expr.bad())
        return result;

    const Expression* base = &expr;
    std::optional<int32_t> index;
    if (expr.kind == ExpressionKind::ElementSelect) {
        auto& es = expr.as<ElementSelectExpression>();
        base = &es.value();
        auto cv = context.tryEval(es.selector());
        if (!cv.isInteger()) {
            context.addDiag(diag::BranchTerminalNotNet, expr.sourceRange);
            return result;
        }
        index = cv.integer().as<int32_t>();
    }

    if (base->kind == ExpressionKind::HierarchicalValue) {
        context.addDiag(diag::VamsNotSupported, expr.sourceRange) << "hierarchical terminals"sv;
        return result;
    }

    if (base->kind != ExpressionKind::NamedValue ||
        base->as<NamedValueExpression>().symbol.kind != SymbolKind::Net) {
        context.addDiag(diag::BranchTerminalNotNet, expr.sourceRange);
        return result;
    }

    auto& net = base->as<NamedValueExpression>().symbol.as<NetSymbol>();
    if (!net.discipline || !net.discipline->isContinuous()) {
        context.addDiag(diag::BranchTerminalNoDiscipline, expr.sourceRange) << net.name;
        return result;
    }

    if (!index && net.getType().getBitWidth() > 1) {
        context.addDiag(diag::VamsNotSupported, expr.sourceRange) << "vector branches"sv;
        return result;
    }

    result.net = &net;
    result.index = index;
    return result;
}

// ----- BranchSymbol -----

void BranchSymbol::fromSyntax(const Scope& scope, const BranchDeclarationSyntax& syntax,
                              SmallVectorBase<const BranchSymbol*>& results) {
    auto& comp = scope.getCompilation();
    for (auto decl : syntax.declarators) {
        if (!decl->dimensions.empty()) {
            scope.addDiag(diag::VamsNotSupported, decl->dimensions.sourceRange())
                << "vector branches"sv;
        }
        auto branch = comp.emplace<BranchSymbol>(decl->name.valueText(), decl->name.location());
        branch->setSyntax(syntax);
        branch->setAttributes(scope, syntax.attributes);
        results.push_back(branch);
    }
}

void BranchSymbol::resolve() const {
    if (resolved)
        return;
    resolved = true;

    auto scope = getParentScope();
    auto syntax = getSyntax();
    SLANG_ASSERT(scope && syntax);
    auto& decl = syntax->as<BranchDeclarationSyntax>();
    if (decl.terminals.empty())
        return;

    // Only the first declarator reports the extra-terminal, same-terminal and compatibility
    // errors, so `branch (a,b) x, y;` reports them once.
    bool first = decl.declarators.empty() || decl.declarators[0]->name.location() == location;
    ASTContext context(*scope, LookupLocation::after(*this), ASTFlags::NonProcedural);

    positive = AnalogTerminal::bind(*decl.terminals[0], context);
    if (decl.terminals.size() > 1)
        negative = AnalogTerminal::bind(*decl.terminals[1], context);
    if (decl.terminals.size() > 2) {
        if (first)
            context.addDiag(diag::BranchTooManyTerminals, decl.terminals[2]->sourceRange());
        return;
    }

    if (!positive.net || (negative && !negative->net))
        return;

    if (negative && positive.sameAs(*negative)) {
        if (first)
            scope->addDiag(diag::BranchSameTerminal, decl.terminals[1]->sourceRange());
        return;
    }

    auto pd = positive.net->discipline;
    if (negative) {
        auto nd = negative->net->discipline;
        if (!pd->isCompatibleWith(*nd)) {
            if (first) {
                scope->addDiag(diag::BranchDisciplineMismatch, decl.terminals[1]->sourceRange())
                    << pd->name << nd->name;
            }
            return;
        }
        // A natureless discipline takes the other terminal's natures.
        if (pd->isNatureless())
            pd = nd;
    }
    discipline = pd;
}

const AnalogTerminal& BranchSymbol::getPositive() const {
    resolve();
    return positive;
}

const AnalogTerminal* BranchSymbol::getNegative() const {
    resolve();
    return negative ? &*negative : nullptr;
}

const DisciplineSymbol* BranchSymbol::getDiscipline() const {
    resolve();
    return discipline;
}

void BranchSymbol::serializeTo(ASTSerializer& serializer) const {
    if (auto& p = getPositive(); p.expr)
        serializer.write("positive", *p.expr);
    if (auto n = getNegative(); n && n->expr)
        serializer.write("negative", *n->expr);
    if (auto d = getDiscipline())
        serializer.writeLink("discipline", *d);
}

} // namespace slang::ast

// ----- SVMake VAMS P0.1 C3: analog blocks, branch access, contributions -----

#include "slang/ast/EvalContext.h"
#include "slang/ast/Lookup.h"
#include "slang/ast/Statement.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/diagnostics/ConstEvalDiags.h"
#include "slang/util/ScopeGuard.h"

namespace slang::ast {

using namespace syntax;
using namespace parsing;

AnalogBlockSymbol& AnalogBlockSymbol::fromSyntax(Scope& scope, const AnalogBlockSyntax& syntax) {
    auto& comp = scope.getCompilation();
    auto result = comp.emplace<AnalogBlockSymbol>(syntax.keyword.location(),
                                                  syntax.initial.kind == TokenKind::InitialKeyword);
    result->setSyntax(syntax);
    result->setAttributes(scope, syntax.attributes);
    result->stmtSyntax = syntax.statement;

    SmallVector<const SyntaxNode*> extraMembers;
    SmallVector<const SyntaxNode*> enumSyntax;
    result->blocks = Statement::createBlockItems(scope, *syntax.statement,
                                                 /* labelHandled */ false, extraMembers,
                                                 enumSyntax);
    for (auto node : enumSyntax)
        scope.addEnumMembers(*node);
    for (auto b : result->blocks)
        scope.addMember(*b);
    for (auto member : extraMembers)
        scope.addMembers(*member);

    return *result;
}

const Statement& AnalogBlockSymbol::getBody() const {
    if (!stmt) {
        SLANG_ASSERT(!isConstructing);
        isConstructing = true;
        auto guard = ScopeGuard([this] { isConstructing = false; });

        auto scope = getParentScope();
        SLANG_ASSERT(scope && stmtSyntax);

        ASTContext context(*scope, LookupLocation::after(*this));
        context.flags |= ASTFlags::AnalogBlock;
        if (isInitial)
            context.flags |= ASTFlags::AnalogInitial;

        Statement::StatementContext stmtCtx(context);
        stmtCtx.blocks = blocks;
        stmt = &Statement::bind(*stmtSyntax, context, stmtCtx);
        if (stmt->bad())
            stmtCtx.blocks = {};
    }
    return *stmt;
}

void AnalogBlockSymbol::serializeTo(ASTSerializer& serializer) const {
    serializer.write("isInitial", isInitial);
    serializer.write("body", getBody());
}

Expression& BranchAccessExpression::fromSyntax(Compilation& comp, std::string_view name,
                                               std::optional<Access> generic,
                                               const InvocationExpressionSyntax& invocation,
                                               const ASTContext& context) {
    auto range = invocation.sourceRange();
    auto bad = [&]() -> Expression& { return badExpr(comp, nullptr); };

    // 5.2.1: an analog initial block has no statements with access functions.
    if (context.flags.has(ASTFlags::AnalogInitial)) {
        context.addDiag(diag::NotAllowedInAnalogInitial, range) << "an access function"sv;
        return bad();
    }

    // 4.4: a branch identifier, or one or two net expressions.
    SmallVector<const ExpressionSyntax*, 2> args;
    if (invocation.arguments) {
        for (auto param : invocation.arguments->parameters) {
            if (param->kind != SyntaxKind::OrderedArgument) {
                context.addDiag(diag::AccessFunctionArgs, param->sourceRange());
                return bad();
            }
            auto expr = context.requireSimpleExpr(*param->as<OrderedArgumentSyntax>().expr);
            if (!expr)
                return bad();
            args.push_back(expr);
        }
    }
    if (args.empty() || args.size() > 2) {
        context.addDiag(diag::AccessFunctionArgs, range);
        return bad();
    }

    const BranchSymbol* branch = nullptr;
    if (args.size() == 1 && args[0]->kind == SyntaxKind::IdentifierName) {
        auto argName = args[0]->as<IdentifierNameSyntax>().identifier.valueText();
        auto sym = Lookup::unqualifiedAt(*context.scope, argName, context.getLocation(),
                                         args[0]->sourceRange());
        if (sym && sym->kind == SymbolKind::Branch)
            branch = &sym->as<BranchSymbol>();
    }

    AnalogTerminal positive;
    std::optional<AnalogTerminal> negative;
    const DisciplineSymbol* discipline = nullptr;
    if (branch) {
        discipline = branch->getDiscipline();
        if (!discipline)
            return bad();
        positive = branch->getPositive();
        if (auto n = branch->getNegative())
            negative = *n;
    }
    else {
        positive = AnalogTerminal::bind(*args[0], context);
        if (!positive.net)
            return bad();
        discipline = positive.net->discipline;

        if (args.size() == 2) {
            negative = AnalogTerminal::bind(*args[1], context);
            if (!negative->net)
                return bad();
            if (positive.sameAs(*negative)) {
                context.addDiag(diag::BranchSameTerminal, args[1]->sourceRange());
                return bad();
            }
            auto nd = negative->net->discipline;
            if (!discipline->isCompatibleWith(*nd)) {
                context.addDiag(diag::BranchDisciplineMismatch, args[1]->sourceRange())
                    << discipline->name << nd->name;
                return bad();
            }
            if (discipline->isNatureless())
                discipline = nd;
        }
    }

    // The access function name comes from the discipline's natures (4.4), never hard-coded.
    Access access = Access::Potential;
    const NatureSymbol* nature = nullptr;
    if (generic) {
        access = *generic;
        nature = access == Access::Potential ? discipline->getPotential() : discipline->getFlow();
    }
    else if (auto p = discipline->getPotential(); p && p->getAccessName() == name) {
        nature = p;
    }
    else if (auto f = discipline->getFlow(); f && f->getAccessName() == name) {
        access = Access::Flow;
        nature = f;
    }

    if (!nature) {
        std::string_view shown = name;
        if (generic)
            shown = *generic == Access::Potential ? "potential"sv : "flow"sv;
        context.addDiag(diag::AccessFunctionMismatch, range) << shown << discipline->name;
        return bad();
    }

    return *comp.emplace<BranchAccessExpression>(comp.getRealType(), access, *nature, *discipline,
                                                 branch, positive, negative, name, range);
}

ConstantValue BranchAccessExpression::evalImpl(EvalContext& context) const {
    context.addDiag(diag::ConstEvalNonConstVariable, sourceRange)
        << (accessName.empty() ? (access == Access::Potential ? "potential"sv : "flow"sv)
                               : accessName);
    return nullptr;
}

bool BranchAccessExpression::isEquivalentImpl(const BranchAccessExpression& rhs) const {
    auto sameTerminal = [](const AnalogTerminal& a, const AnalogTerminal& b) {
        return a.net == b.net && a.index == b.index;
    };
    if (access != rhs.access || nature != rhs.nature || branch != rhs.branch)
        return false;
    if (!sameTerminal(positive, rhs.positive) || negative.has_value() != rhs.negative.has_value())
        return false;
    return !negative || sameTerminal(*negative, *rhs.negative);
}

void BranchAccessExpression::serializeTo(ASTSerializer& serializer) const {
    serializer.write("access", access == Access::Potential ? "potential"sv : "flow"sv);
    serializer.writeLink("nature", *nature);
    serializer.writeLink("discipline", *discipline);
    if (branch)
        serializer.writeLink("branch", *branch);
    if (positive.expr)
        serializer.write("positive", *positive.expr);
    if (negative && negative->expr)
        serializer.write("negative", *negative->expr);
}

Statement& ContributionStatement::fromSyntax(Compilation& comp,
                                             const ContributionStatementSyntax& syntax,
                                             const ASTContext& context) {
    // analog initial takes an analog_function_statement, which has no contributions (5.2.1);
    // report that before the target's own analog-initial error.
    if (context.flags.has(ASTFlags::AnalogInitial) && context.flags.has(ASTFlags::AnalogBlock)) {
        context.addDiag(diag::ContributionInAnalogInitial, syntax.op.range());
        auto& lhs = *comp.emplace<InvalidExpression>(nullptr, comp.getErrorType());
        return badStmt(comp, comp.emplace<ContributionStatement>(lhs, lhs, syntax.sourceRange()));
    }

    auto& lhs = Expression::bind(*syntax.lhs, context);
    auto& rhs = Expression::bindRValue(comp.getRealType(), *syntax.rhs, syntax.op.range(),
                                       context);
    auto result = comp.emplace<ContributionStatement>(lhs, rhs, syntax.sourceRange());
    if (lhs.bad() || rhs.bad())
        return badStmt(comp, result);

    // An analog_expression is numeric; a string literal would convert as a bit vector.
    if (rhs.unwrapImplicitConversions().kind == ExpressionKind::StringLiteral ||
        !rhs.unwrapImplicitConversions().type->isNumeric()) {
        context.addDiag(diag::ContributionValueNotNumeric, rhs.sourceRange);
        return badStmt(comp, result);
    }

    // 5.6: the target is a branch access function call (branch_lvalue).
    if (lhs.kind != ExpressionKind::BranchAccess) {
        context.addDiag(diag::ContributionTargetNotBranch, lhs.sourceRange);
        return badStmt(comp, result);
    }

    if (!context.flags.has(ASTFlags::AnalogBlock)) {
        context.addDiag(diag::ContributionOutsideAnalog, syntax.op.range());
        return badStmt(comp, result);
    }

    if (context.flags.has(ASTFlags::AnalogEvent)) {
        context.addDiag(diag::ContributionInEventStatement, syntax.op.range());
        return badStmt(comp, result);
    }

    // analog initial takes an analog_function_statement, which has no contributions.
    if (context.flags.has(ASTFlags::AnalogInitial)) {
        context.addDiag(diag::ContributionInAnalogInitial, syntax.op.range());
        return badStmt(comp, result);
    }

    return *result;
}

Statement::EvalResult ContributionStatement::evalImpl(EvalContext& context) const {
    context.addDiag(diag::ConstEvalTimedStmtNotConst, sourceRange);
    return EvalResult::Fail;
}

void ContributionStatement::serializeTo(ASTSerializer& serializer) const {
    serializer.write("lhs", lhs);
    serializer.write("rhs", rhs);
}

} // namespace slang::ast
