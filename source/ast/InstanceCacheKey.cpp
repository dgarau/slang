//------------------------------------------------------------------------------
// InstanceCacheKey.cpp
// Hash key for instance symbols
//
// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#include "slang/ast/InstanceCacheKey.h"

#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/ast/types/Type.h"

namespace slang::ast {

bool InstanceCacheKey::isEligibleForCaching(const InstanceSymbol& symbol) {
    return !symbol.resolvedConfig && !symbol.body.hierarchyOverrideNode &&
           !symbol.body.flags.has(InstanceFlags::PreventsCaching);
}

InstanceCacheKey::InstanceCacheKey(const InstanceSymbol& symbol, bool& valid,
                                   SmallSet<const InstanceSymbol*, 2>& visitedInstances) :
    symbol(&symbol) {
    size_t h = 0;
    hash_combine(h, &symbol.getDefinition());

    // Localparams are left out. Their values follow from the other parameters,
    // and resolving one here could need this very instance, such as a
    // localparam naming a virtual interface of the interface that declares it.
    for (auto param : symbol.body.getParameters()) {
        if (param->isLocalParam())
            continue;

        if (param->symbol.kind == SymbolKind::Parameter) {
            // The parameter's type is hashed alongside its value: isIdentical (below) tells apart
            // two integers of different width or signedness, but the declared or inferred type
            // can still differ between values that are identical (a two-state and a four-state
            // type of one width, say), and the body elaborates with that type.
            auto& param_symbol = param->symbol.as<ParameterSymbol>();
            hash_combine(h, param_symbol.getValue().hash());
            hash_combine(h, param_symbol.getType().hash());
            param_symbol.getType().appendMemberDefaults(memberDefaults);
        }
        else {
            auto& type = param->symbol.as<TypeParameterSymbol>().targetType.getType();
            hash_combine(h, type.hash());
            type.appendMemberDefaults(memberDefaults);
        }
    }

    // Two types the comparison below accepts as interchangeable (copies of one declaration,
    // or identical structs) can still differ in their default member values (7.2.2), which
    // the body can read at elaboration (13.4.3): those bodies must not be shared (6.22,
    // 23.10). The defaults are evaluated here rather than in operator==, which runs inside
    // the cache's insert, where binding an initializer could re-enter it (SVMake K-2445).
    for (auto& entry : memberDefaults)
        hash_combine(h, entry.value ? entry.value->hash() : 0);

    for (auto conn : symbol.getPortConnections()) {
        if (conn->port.kind == SymbolKind::InterfacePort) {
            auto [iface, modport] = conn->getIfaceConn();
            while (iface && iface->kind == SymbolKind::InstanceArray) {
                auto& arr = iface->as<InstanceArraySymbol>();
                if (arr.elements.empty())
                    iface = nullptr;
                else
                    iface = arr.elements[0];
            }

            if (iface) {
                auto& ifaceInst = iface->as<InstanceSymbol>();
                if (!isEligibleForCaching(ifaceInst) ||
                    !visitedInstances.insert(&ifaceInst).second) {
                    valid = false;
                    return;
                }

                InstanceCacheKey ifaceKey(ifaceInst, valid, visitedInstances);
                if (!valid)
                    return;

                ifaceKeys.emplace_back(std::move(ifaceKey), modport);
                hash_combine(h, ifaceKeys.back().first.savedHash);

                if (modport)
                    hash_combine(h, modport->name);
            }
        }
    }

    savedHash = h;
}

bool InstanceCacheKey::operator==(const InstanceCacheKey& other) const {
    if (savedHash != other.savedHash ||
        &symbol->getDefinition() != &other.symbol->getDefinition() ||
        ifaceKeys.size() != other.ifaceKeys.size()) {
        return false;
    }

    auto lparams = symbol->body.getParameters();
    auto rparams = other.symbol->body.getParameters();
    SLANG_ASSERT(lparams.size() == rparams.size());

    for (size_t i = 0; i < lparams.size(); i++) {
        auto lp = lparams[i];
        auto rp = rparams[i];
        SLANG_ASSERT(lp->symbol.kind == rp->symbol.kind);
        if (lp->isLocalParam())
            continue;

        if (lp->symbol.kind == SymbolKind::Parameter) {
            // Values that compare equal can still behave differently in the body.
            auto& lps = lp->symbol.as<ParameterSymbol>();
            auto& rps = rp->symbol.as<ParameterSymbol>();
            if (!lps.getValue().isIdentical(rps.getValue()))
                return false;

            // See the constructor for why the type is compared too.
            auto& lpt = lps.getType();
            auto& rpt = rps.getType();
            if (!lpt.isMatchingAcrossElaborations(rpt) && !lpt.isIdenticalStructUnion(rpt))
                return false;
        }
        else {
            auto& lt = lp->symbol.as<TypeParameterSymbol>().targetType.getType();
            auto& rt = rp->symbol.as<TypeParameterSymbol>().targetType.getType();
            if (!lt.isMatchingAcrossElaborations(rt) && !lt.isIdenticalStructUnion(rt))
                return false;
        }
    }

    if (!Type::sameMemberDefaults(memberDefaults, other.memberDefaults))
        return false;

    for (size_t i = 0; i < ifaceKeys.size(); i++) {
        auto& l = ifaceKeys[i];
        auto& r = other.ifaceKeys[i];
        if (l.first != r.first)
            return false;

        if (bool(l.second) != bool(r.second) || (l.second && l.second->name != r.second->name))
            return false;
    }

    return true;
}

} // namespace slang::ast
