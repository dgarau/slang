//------------------------------------------------------------------------------
// DriverTracker.cpp
// Centralized tracking of assigned / driven symbols
//
// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#include "slang/analysis/DriverTracker.h"

#include "slang/analysis/AnalysisManager.h"
#include "slang/analysis/AnalyzedProcedure.h"
#include "slang/ast/ASTVisitor.h"
#include "slang/ast/EvalContext.h"
#include "slang/ast/LSPUtilities.h"
#include "slang/diagnostics/AnalysisDiags.h"

namespace slang::analysis {

using namespace ast;

void DriverTracker::add(AnalysisContext& context, DriverAlloc& driverAlloc,
                        const AnalyzedProcedure& procedure) {
    SmallVector<std::pair<const HierarchicalReference*, const ValueDriver*>> ifacePortRefs;
    for (auto& [valueSym, drivers] : procedure.getDrivers()) {
        auto updateFunc = [&](auto& elem) {
            for (auto& [driver, bounds] : drivers) {
                auto ref = addDriver(context, driverAlloc, *elem.first, elem.second, *driver,
                                     bounds);
                if (ref) {
                    // This driver is via an interface port so we need to
                    // store and then apply it after we're done touching the
                    // symbolDrivers map.
                    ifacePortRefs.emplace_back(ref, driver);
                }
            }
        };
        symbolDrivers.try_emplace_and_visit(valueSym, updateFunc, updateFunc);
    }

    for (auto& [ref, driver] : ifacePortRefs)
        noteInterfacePortDriver(context, driverAlloc, *ref, *driver);
}

void DriverTracker::add(AnalysisContext& context, DriverAlloc& driverAlloc,
                        const PortConnection& connection, const Symbol& containingSymbol) {
    auto& port = connection.port;
    auto expr = connection.getExpression();
    if (!expr || expr->bad() || port.kind == SymbolKind::InterfacePort)
        return;

    ArgumentDirection direction;
    if (port.kind == SymbolKind::Port)
        direction = port.as<PortSymbol>().direction;
    else
        direction = port.as<MultiPortSymbol>().direction;

    // Input ports are not drivers.
    if (direction == ArgumentDirection::In)
        return;

    bitmask<DriverFlags> flags;
    if (direction == ArgumentDirection::Out)
        flags = DriverFlags::OutputPort;

    if (expr->kind == ExpressionKind::Assignment)
        expr = &expr->as<AssignmentExpression>().left();

    addDrivers(context, driverAlloc, *expr, DriverKind::Continuous, flags, containingSymbol);
}

void DriverTracker::add(AnalysisContext& context, DriverAlloc& driverAlloc,
                        const PortSymbol& symbol) {
    // This method adds driver *from* the port to the *internal*
    // symbol (or expression) that it connects to.
    auto dir = symbol.direction;
    if (dir != ArgumentDirection::In && dir != ArgumentDirection::InOut)
        return;

    auto flags = dir == ArgumentDirection::In ? DriverFlags::InputPort : DriverFlags::None;
    auto scope = symbol.getParentScope();
    SLANG_ASSERT(scope);

    if (auto expr = symbol.getInternalExpr()) {
        addDrivers(context, driverAlloc, *expr, DriverKind::Continuous, flags, scope->asSymbol());
    }
    else if (auto is = symbol.internalSymbol) {
        auto nve = context.alloc.emplace<NamedValueExpression>(
            is->as<ValueSymbol>(), SourceRange{is->location, is->location + is->name.length()});
        addDrivers(context, driverAlloc, *nve, DriverKind::Continuous, flags, scope->asSymbol());
    }
}

void DriverTracker::add(AnalysisContext& context, DriverAlloc& driverAlloc,
                        const ClockVarSymbol& symbol) {
    // Input clock vars don't have drivers.
    if (symbol.direction == ArgumentDirection::In)
        return;

    auto scope = symbol.getParentScope();
    SLANG_ASSERT(scope);

    if (auto expr = symbol.getInitializer()) {
        addDrivers(context, driverAlloc, *expr, DriverKind::Continuous, DriverFlags::ClockVar,
                   scope->asSymbol());
    }
}

void DriverTracker::add(AnalysisContext& context, DriverAlloc& driverAlloc, const Expression& expr,
                        const Symbol& containingSymbol) {
    addDrivers(context, driverAlloc, expr, DriverKind::Continuous, DriverFlags::None,
               containingSymbol);
}

void DriverTracker::add(AnalysisContext& context, DriverAlloc& driverAlloc,
                        std::span<const SymbolDriverListPair> symbolDriverList) {
    for (auto& [valueSym, drivers] : symbolDriverList) {
        auto updateFunc = [&](auto& elem) {
            for (auto& [driver, bounds] : drivers) {
                auto ref = addDriver(context, driverAlloc, *elem.first, elem.second, *driver,
                                     bounds);
                SLANG_ASSERT(!ref);
            }
        };
        symbolDrivers.try_emplace_and_visit(valueSym, updateFunc, updateFunc);
    }
}

void DriverTracker::noteNonCanonicalInstance(AnalysisContext& context, DriverAlloc& driverAlloc,
                                             const InstanceSymbol& instance) {
    auto canonical = instance.getCanonicalBody();
    SLANG_ASSERT(canonical);

    std::vector<InstanceState::IfacePortDriver> ifacePortDrivers;
    auto updater = [&](auto& item) {
        auto& state = item.second;
        state.nonCanonicalInstances.push_back(&instance);

        // Copy these out so we can act on them outside of the concurrent visitor.
        ifacePortDrivers = state.ifacePortDrivers;
    };
    instanceMap.try_emplace_and_visit(canonical, updater, updater);

    for (auto& ifacePortDriver : ifacePortDrivers)
        applyInstanceSideEffect(context, driverAlloc, ifacePortDriver, instance);
}

void DriverTracker::propagateModportDrivers(AnalysisContext& context, DriverAlloc& driverAlloc) {
    while (true) {
        concurrent_map<const ast::ValueSymbol*, DriverList> localCopy;
        std::swap(modportPortDrivers, localCopy);
        if (localCopy.empty())
            break;

        localCopy.cvisit_all([&](auto& item) {
            if (auto expr = item.first->template as<ModportPortSymbol>().getConnectionExpr()) {
                for (auto& [originalDriver, _] : item.second)
                    propagateModportDriver(context, driverAlloc, *expr, *originalDriver);
            }
        });
    }
}

void DriverTracker::propagateModportDriver(AnalysisContext& context, DriverAlloc& driverAlloc,
                                           const Expression& connectionExpr,
                                           const ValueDriver& originalDriver) {
    // TODO: this is clunky, but we need to be able to glue the outer select
    // expression to the inner connection expression. Probably the expression AST
    // should have a way to do this generically.
    //
    // For now this works -- the const_casts are sus but we never mutate anything
    // during analysis so it works out.
    const Expression* initialLSP = nullptr;
    switch (originalDriver.prefixExpression->kind) {
        case ExpressionKind::ElementSelect: {
            auto& es = originalDriver.prefixExpression->as<ElementSelectExpression>();
            initialLSP = context.alloc.emplace<ElementSelectExpression>(
                *es.type, const_cast<Expression&>(connectionExpr), es.selector(), es.sourceRange);
            break;
        }
        case ExpressionKind::RangeSelect: {
            auto& rs = originalDriver.prefixExpression->as<RangeSelectExpression>();
            initialLSP = context.alloc.emplace<RangeSelectExpression>(
                rs.getSelectionKind(), *rs.type, const_cast<Expression&>(connectionExpr), rs.left(),
                rs.right(), rs.sourceRange);
            break;
        }
        case ExpressionKind::MemberAccess: {
            auto& ma = originalDriver.prefixExpression->as<MemberAccessExpression>();
            initialLSP = context.alloc.emplace<MemberAccessExpression>(
                *ma.type, const_cast<Expression&>(connectionExpr), ma.member, ma.sourceRange);
            break;
        }
        default:
            break;
    }

    addDrivers(context, driverAlloc, connectionExpr, originalDriver.kind, originalDriver.flags,
               *originalDriver.containingSymbol, initialLSP);
}

void DriverTracker::addDrivers(AnalysisContext& context, DriverAlloc& driverAlloc,
                               const Expression& expr, DriverKind driverKind,
                               bitmask<DriverFlags> driverFlags, const Symbol& containingSymbol,
                               const Expression* initialLSP) {
    EvalContext evalCtx(containingSymbol);
    SmallVector<std::pair<const HierarchicalReference*, const ValueDriver*>> ifacePortRefs;
    LSPUtilities::visitLSPs(
        expr, evalCtx,
        [&](const ValueSymbol& symbol, const Expression& lsp, bool isLValue) {
            // If this is not an lvalue, we don't care about it.
            if (!isLValue)
                return;

            auto bounds = LSPUtilities::getBounds(lsp, evalCtx, symbol.getType());
            if (!bounds)
                return;

            auto driver = context.alloc.emplace<ValueDriver>(driverKind, lsp, containingSymbol,
                                                             driverFlags);

            auto updateFunc = [&](auto& elem) {
                if (auto ref = addDriver(context, driverAlloc, *elem.first, elem.second, *driver,
                                         *bounds)) {
                    ifacePortRefs.emplace_back(ref, driver);
                }
            };
            symbolDrivers.try_emplace_and_visit(&symbol, updateFunc, updateFunc);
        },
        initialLSP);

    for (auto& [ref, driver] : ifacePortRefs)
        noteInterfacePortDriver(context, driverAlloc, *ref, *driver);
}

DriverList DriverTracker::getDrivers(const ValueSymbol& symbol) const {
    DriverList drivers;
    symbolDrivers.cvisit(&symbol, [&drivers](auto& item) {
        for (auto it = item.second.begin(); it != item.second.end(); ++it)
            drivers.emplace_back(*it, it.bounds());
    });
    return drivers;
}

static std::string getLSPName(const ValueSymbol& symbol, const ValueDriver& driver) {
    FormatBuffer buf;
    EvalContext evalContext(symbol);
    LSPUtilities::stringifyLSP(*driver.prefixExpression, evalContext, buf);
    return buf.str();
}

static bool handleOverlap(AnalysisContext& context, const ValueSymbol& symbol,
                          const ValueDriver& curr, const ValueDriver& driver, bool isNet,
                          bool isUWire, bool isSingleDriverUDNT, const NetType* netType) {
    auto currRange = curr.getSourceRange();
    auto driverRange = driver.getSourceRange();

    // The default handling case for mixed vs multiple assignments is below.
    // First check for more specialized cases here:
    // 1. If this is a non-uwire net for an input or output port
    // 2. If this is a variable for an input port
    const bool isUnidirectionNetPort = isNet && (curr.isUnidirectionalPort() ||
                                                 driver.isUnidirectionalPort());

    if ((isUnidirectionNetPort && !isUWire && !isSingleDriverUDNT) ||
        (!isNet && (curr.isInputPort() || driver.isInputPort()))) {
        auto code = diag::InputPortAssign;
        if (isNet) {
            if (curr.flags.has(DriverFlags::InputPort))
                code = diag::InputPortCoercion;
            else
                code = diag::OutputPortCoercion;
        }

        // This is a little messy; basically we want to report the correct
        // range for the port vs the assignment. We only want to do this
        // for input ports though, as output ports show up at the instantiation
        // site and we'd rather that be considered the "port declaration".
        auto portRange = currRange;
        auto assignRange = driverRange;
        if (driver.isInputPort() || curr.flags.has(DriverFlags::OutputPort))
            std::swap(portRange, assignRange);

        auto& diag = context.addDiag(symbol, code, assignRange);
        diag << symbol.name;

        auto note = code == diag::OutputPortCoercion ? diag::NoteDrivenHere
                                                     : diag::NoteDeclarationHere;
        diag.addNote(note, portRange);

        // For variable ports this is an error, for nets it's a warning.
        return isNet;
    }

    if (curr.isClockVar() || driver.isClockVar()) {
        // Both drivers being clockvars is allowed.
        if (curr.isClockVar() && driver.isClockVar())
            return true;

        // Procedural drivers are allowed to clockvars.
        if (curr.kind == DriverKind::Procedural || driver.kind == DriverKind::Procedural)
            return true;

        // Otherwise we have an error.
        if (driver.isClockVar())
            std::swap(driverRange, currRange);

        auto& diag = context.addDiag(symbol, diag::ClockVarTargetAssign, driverRange);
        diag << symbol.name;
        diag.addNote(diag::NoteReferencedHere, currRange);
        return false;
    }

    auto addAssignedHereNote = [&](Diagnostic& d) {
        // If the two locations are the same, the symbol is driven by
        // the same source location but two different parts of the hierarchy.
        // In those cases we want a different note about what's going on.
        if (currRange.start() != driverRange.start()) {
            d.addNote(diag::NoteAssignedHere, currRange);
        }
        else {
            auto& note = d.addNote(diag::NoteFromHere2, SourceLocation::NoLocation);
            note << driver.containingSymbol->getHierarchicalPath();
            note << curr.containingSymbol->getHierarchicalPath();
        }
    };

    if (curr.kind == DriverKind::Procedural && driver.kind == DriverKind::Procedural) {
        // Multiple procedural drivers where one of them is an
        // always_comb / always_ff block.
        ProceduralBlockKind procKind;
        const ValueDriver* sourceForName = &driver;
        if (driver.isInSingleDriverProcedure()) {
            procKind = static_cast<ProceduralBlockKind>(driver.source);
        }
        else {
            procKind = static_cast<ProceduralBlockKind>(curr.source);
            std::swap(driverRange, currRange);
            sourceForName = &curr;
        }

        auto& diag = context.addDiag(symbol, diag::MultipleAlwaysAssigns, driverRange);
        diag << getLSPName(symbol, *sourceForName) << SemanticFacts::getProcedureKindStr(procKind);
        addAssignedHereNote(diag);

        if (driver.procCallExpression || curr.procCallExpression) {
            SourceRange extraRange = driver.procCallExpression
                                         ? driver.prefixExpression->sourceRange
                                         : curr.prefixExpression->sourceRange;

            diag.addNote(diag::NoteOriginalAssign, extraRange);
        }

        return false;
    }

    DiagCode code;
    if (isUWire)
        code = diag::MultipleUWireDrivers;
    else if (isSingleDriverUDNT)
        code = diag::MultipleUDNTDrivers;
    else if (driver.kind == DriverKind::Continuous && curr.kind == DriverKind::Continuous)
        code = diag::MultipleContAssigns;
    else
        code = diag::MixedVarAssigns;

    auto& diag = context.addDiag(symbol, code, driverRange);
    diag << getLSPName(symbol, driver);
    if (isSingleDriverUDNT) {
        SLANG_ASSERT(netType);
        diag << netType->name;
    }

    addAssignedHereNote(diag);
    return false;
}

const HierarchicalReference* DriverTracker::addDriver(
    AnalysisContext& context, DriverAlloc& driverAlloc, const ValueSymbol& symbol,
    SymbolDriverMap& driverMap, const ValueDriver& driver, DriverBitRange bounds) {

    // Class types don't have drivers, so we can skip this.
    if (symbol.getDeclaredType()->getType().isClass()) {
        return nullptr;
    }

    auto scope = symbol.getParentScope();
    SLANG_ASSERT(scope);

    // If this driver is made via an interface port connection we want to
    // note that fact as it represents a side effect for the instance that
    // is not captured in the port connections.
    const HierarchicalReference* result = nullptr;
    if (!driver.isFromSideEffect) {
        LSPUtilities::visitComponents(
            *driver.prefixExpression, /* includeRoot */ true, [&](const Expression& expr) {
                if (expr.kind == ExpressionKind::HierarchicalValue) {
                    auto& ref = expr.as<HierarchicalValueExpression>().ref;
                    if (ref.isViaIfacePort())
                        result = &ref;
                }
            });
    }

    // Keep track of modport ports so we can revisit them at the end of analysis.
    if (symbol.kind == SymbolKind::ModportPort) {
        auto updater = [&](auto& item) { item.second.emplace_back(&driver, bounds); };
        modportPortDrivers.try_emplace_and_visit(&symbol, updater, updater);
        return result;
    }

    if (driverMap.empty()) {
        // The first time we add a driver, check whether there is also an
        // initializer expression that should count as a driver as well.
        auto addInitializer = [&](DriverKind driverKind) {
            auto& valExpr = *context.alloc.emplace<NamedValueExpression>(
                symbol, SourceRange{symbol.location, symbol.location + symbol.name.length()});

            DriverBitRange initBounds{0, symbol.getType().getSelectableWidth() - 1};
            auto initDriver = context.alloc.emplace<ValueDriver>(driverKind, valExpr,
                                                                 scope->asSymbol(),
                                                                 DriverFlags::Initializer);

            driverMap.insert(initBounds, initDriver, driverAlloc);
        };

        switch (symbol.kind) {
            case SymbolKind::Net:
                if (symbol.getInitializer())
                    addInitializer(DriverKind::Continuous);
                break;
            case SymbolKind::Variable:
            case SymbolKind::ClassProperty:
            case SymbolKind::Field:
                if (symbol.getInitializer())
                    addInitializer(DriverKind::Procedural);
                break;
            default:
                break;
        }

        if (driverMap.empty()) {
            driverMap.insert(bounds, &driver, driverAlloc);
            return result;
        }
    }

    // We need to check for overlap in the following cases:
    // - static variables (automatic variables can't ever be driven continuously)
    // - uwire nets
    // - user-defined nets with no resolution function
    const bool isNet = symbol.kind == SymbolKind::Net;
    bool isUWire = false;
    bool isSingleDriverUDNT = false;
    const NetType* netType = nullptr;

    if (isNet) {
        netType = &symbol.as<NetSymbol>().netType;
        isUWire = netType->netKind == NetType::UWire;
        isSingleDriverUDNT = netType->netKind == NetType::UserDefined &&
                             netType->getResolutionFunction() == nullptr;
    }

    const bool checkOverlap = (VariableSymbol::isKind(symbol.kind) &&
                               symbol.as<VariableSymbol>().lifetime == VariableLifetime::Static) ||
                              isUWire || isSingleDriverUDNT ||
                              symbol.kind == SymbolKind::LocalAssertionVar;

    const bool allowDupInitialDrivers = context.manager->hasFlag(
        AnalysisFlags::AllowDupInitialDrivers);

    auto shouldIgnore = [&](const ValueDriver& vd) {
        // We ignore drivers from subroutines and from initializers.
        // We also ignore initial blocks if the user has set a flag.
        return vd.source == DriverSource::Subroutine || vd.flags.has(DriverFlags::Initializer) ||
               (vd.source == DriverSource::Initial && allowDupInitialDrivers);
    };

    // TODO: try to clean these conditions up a bit more
    auto end = driverMap.end();
    for (auto it = driverMap.find(bounds); it != end; ++it) {
        // Check whether this pair of drivers overlapping constitutes a problem.
        // The conditions for reporting a problem are:
        // - If this is for a mix of input/output and inout ports, always report.
        // - Don't report for "Other" drivers (procedural force / release, etc)
        // - Otherwise, if is this a static var or uwire net:
        //      - Report if a mix of continuous and procedural assignments
        //      - Don't report if both drivers are sliced ports from an array
        //        of instances. We already sliced these up correctly when the
        //        connections were made and the overlap logic here won't work correctly.
        //      - Report if multiple continuous assignments
        //      - If both procedural, report if there aren multiple
        //        always_comb / always_ff procedures.
        //          - If the allowDupInitialDrivers option is set, allow an initial
        //            block to overlap even if the other block is an always_comb/ff.
        // - Assertion local variable formal arguments can't drive more than
        //   one output to the same local variable.
        bool isProblem = false;
        auto curr = *it;

        if (curr->isUnidirectionalPort() != driver.isUnidirectionalPort()) {
            isProblem = true;
        }
        else if (checkOverlap) {
            if (driver.kind == DriverKind::Continuous || curr->kind == DriverKind::Continuous) {
                isProblem = true;
            }
            else if (curr->containingSymbol != driver.containingSymbol && !shouldIgnore(*curr) &&
                     !shouldIgnore(driver) &&
                     (curr->isInSingleDriverProcedure() || driver.isInSingleDriverProcedure())) {
                isProblem = true;
            }
        }

        if (isProblem) {
            if (!handleOverlap(context, symbol, *curr, driver, isNet, isUWire, isSingleDriverUDNT,
                               netType)) {
                break;
            }
        }
    }

    driverMap.insert(bounds, &driver, driverAlloc);
    return result;
}

void DriverTracker::noteInterfacePortDriver(AnalysisContext& context, DriverAlloc& driverAlloc,
                                            const HierarchicalReference& ref,
                                            const ValueDriver& driver) {
    SLANG_ASSERT(ref.isViaIfacePort());
    SLANG_ASSERT(ref.target);
    SLANG_ASSERT(ref.expr);

    auto& port = ref.path[0].symbol->as<InterfacePortSymbol>();
    auto scope = port.getParentScope();
    SLANG_ASSERT(scope);

    auto& symbol = scope->asSymbol();
    SLANG_ASSERT(symbol.kind == SymbolKind::InstanceBody);

    InstanceState::IfacePortDriver ifacePortDriver{&ref, &driver};
    std::vector<const ast::InstanceSymbol*> nonCanonicalInstances;
    auto updater = [&](auto& item) {
        auto& state = item.second;
        state.ifacePortDrivers.push_back(ifacePortDriver);

        // Copy these out so we can act on them outside of the concurrent visitor.
        nonCanonicalInstances = state.nonCanonicalInstances;
    };
    instanceMap.try_emplace_and_visit(&symbol.as<InstanceBodySymbol>(), updater, updater);

    for (auto inst : nonCanonicalInstances)
        applyInstanceSideEffect(context, driverAlloc, ifacePortDriver, *inst);

    // If this driver's target is through another interface port we should
    // recursively follow it to the parent connection.
    auto [_, expr] = port.getConnectionAndExpr();
    if (expr && expr->kind == ExpressionKind::ArbitrarySymbol) {
        auto& connRef = expr->as<ArbitrarySymbolExpression>().hierRef;
        if (connRef.isViaIfacePort())
            noteInterfacePortDriver(context, driverAlloc, connRef.join(context.alloc, ref), driver);
    }
}

static const Symbol* retargetIfacePort(const HierarchicalReference& ref,
                                       const InstanceSymbol& base) {
    // This function retargets a hierarchical reference that begins with
    // an interface port access to a different instance that has the same port,
    // i.e. performing the same lookup for a different but identical instance.
    if (!ref.isViaIfacePort() || !ref.target)
        return nullptr;

    // Should always find the port here unless some other error occurred.
    auto& path = ref.path;
    auto port = base.body.findPort(path[0].symbol->name);
    if (!port)
        return nullptr;

    const Symbol* symbol = port;
    const ModportSymbol* modport = nullptr;
    std::optional<std::span<const Symbol* const>> instanceArrayElems;

    // Walk the path to find the target symbol.
    for (size_t i = 1; i < path.size(); i++) {
        while (symbol && symbol->kind == SymbolKind::InterfacePort)
            std::tie(symbol, modport) = symbol->as<InterfacePortSymbol>().getConnection();

        if (!symbol)
            return nullptr;

        // instanceArrayElems is valid when the prior entry in the path
        // did a range select of an interface instance array. We don't
        // have a way to represent that range as a symbol, so we track this
        // as a separate optional span of selected instances.
        if (!instanceArrayElems) {
            if (symbol->kind == SymbolKind::Instance) {
                auto& body = symbol->as<InstanceSymbol>().body;
                symbol = &body;

                // We should never see a module instance on this path
                // unless there is an error, because modules can't be
                // instantiated in interfaces,
                if (body.getDefinition().definitionKind == DefinitionKind::Module)
                    return nullptr;

                // See lookupDownward in Lookup.cpp for the logic here.
                if (modport) {
                    symbol = body.find(modport->name);
                    modport = nullptr;
                    SLANG_ASSERT(symbol);
                }
            }
            else if (symbol->kind == SymbolKind::InstanceArray) {
                instanceArrayElems = symbol->as<InstanceArraySymbol>().elements;
            }
            else if (!symbol->isScope()) {
                return nullptr;
            }
        }

        auto& elem = path[i];
        if (auto index = std::get_if<int32_t>(&elem.selector)) {
            // We're doing an element select here.
            if (instanceArrayElems) {
                // Prior entry was a range select, so select further within it.
                if (*index < 0 || size_t(*index) >= instanceArrayElems->size())
                    return nullptr;

                symbol = (*instanceArrayElems)[size_t(*index)];
            }
            else if (symbol->kind == SymbolKind::GenerateBlockArray) {
                auto& arr = symbol->as<GenerateBlockArraySymbol>();
                if (!arr.valid || *index < 0 || size_t(*index) >= arr.entries.size())
                    return nullptr;

                symbol = arr.entries[size_t(*index)];
            }
            else {
                return nullptr;
            }
        }
        else if (auto range = std::get_if<std::pair<int32_t, int32_t>>(&elem.selector)) {
            // We're doing a range select here.
            if (!instanceArrayElems)
                return nullptr;

            auto size = instanceArrayElems->size();
            if (range->first < 0 || size_t(range->second) >= size)
                return nullptr;

            if (size_t(range->first) >= size || size_t(range->second) >= size)
                return nullptr;

            // We `continue` here so that we don't reset the instanceArrayElems.
            instanceArrayElems = instanceArrayElems->subspan(
                size_t(range->first), size_t(range->second - range->first) + 1);
            continue;
        }
        else {
            auto name = std::get<std::string_view>(elem.selector);
            auto next = symbol->as<Scope>().find(name);
            if (!next && symbol->kind == SymbolKind::Modport) {
                // See lookupDownward in Lookup.cpp for the logic here.
                next = symbol->getParentScope()->find(name);
                if (!next || SemanticFacts::isAllowedInModport(next->kind) ||
                    next->kind == SymbolKind::Modport) {
                    return nullptr;
                }
            }
            symbol = next;
        }

        // Otherwise we're done with the range select if we had one.
        instanceArrayElems.reset();
    }

    return symbol;
}

void DriverTracker::applyInstanceSideEffect(AnalysisContext& context, DriverAlloc& driverAlloc,
                                            const InstanceState::IfacePortDriver& ifacePortDriver,
                                            const InstanceSymbol& instance) {
    auto& ref = *ifacePortDriver.ref;
    if (auto target = retargetIfacePort(ref, instance)) {
        auto driver = context.alloc.emplace<ValueDriver>(*ifacePortDriver.driver);
        driver->containingSymbol = &instance;
        driver->isFromSideEffect = true;

        EvalContext evalCtx(instance);
        auto& valueSym = target->as<ValueSymbol>();
        auto bounds = LSPUtilities::getBounds(*driver->prefixExpression, evalCtx,
                                              valueSym.getType());
        if (!bounds)
            return;

        auto updateFunc = [&](auto& elem) {
            auto ref = addDriver(context, driverAlloc, *elem.first, elem.second, *driver, *bounds);
            SLANG_ASSERT(!ref);
        };
        symbolDrivers.try_emplace_and_visit(&valueSym, updateFunc, updateFunc);
    }
}

} // namespace slang::analysis
