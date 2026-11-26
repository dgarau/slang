//------------------------------------------------------------------------------
// AnalogFuncs.cpp
// SVMake VAMS P0.1: Verilog-AMS (Accellera VAMS-2023) built-in functions
//
// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#include "Builtins.h"
#include <cmath>
#include <fmt/format.h>

#include "slang/ast/ASTContext.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/EvalContext.h"
#include "slang/ast/SystemSubroutine.h"
#include "slang/ast/expressions/LiteralExpressions.h"
#include "slang/ast/types/Type.h"
#include "slang/diagnostics/VamsDiags.h"

namespace slang::ast::builtins {

using parsing::TokenKind;

// The analog operators (VAMS-2023 4.5) are legal only in an analog block, not in
// analog initial (whose statements are analog_function_statements).
static bool requireAnalogBlock(const ASTContext& context, std::string_view name,
                               SourceRange range) {
    if (!context.flags.has(ASTFlags::AnalogBlock) || context.flags.has(ASTFlags::AnalogInitial)) {
        context.addDiag(diag::AnalogOperatorOutsideAnalog, range) << name;
        return false;
    }
    return true;
}

class AnalogBuiltin : public SystemSubroutine {
protected:
    AnalogBuiltin(std::string_view name, AnalogBuiltinKind kind) :
        SystemSubroutine(std::string(name), SubroutineKind::Function) {
        analogKind = kind;
    }

    bool checkNumericArgs(const ASTContext& context, const Args& args) const {
        for (auto arg : args) {
            if (arg->kind == ExpressionKind::EmptyArgument)
                continue;
            if (!arg->type->isNumeric()) {
                badArg(context, *arg);
                return false;
            }
        }
        return true;
    }
};

// ln, log (base 10), exp, sqrt, min, max, abs, pow, floor, ceil, the trigonometric and
// hyperbolic functions, atan2, hypot, ln1p, expm1 (VAMS-2023 4.3.1, Table 4-14).
// Constant-evaluable, and legal anywhere in Verilog-AMS code.
class AnalogMathFunction : public AnalogBuiltin {
public:
    using F1 = double (*)(double);
    using F2 = double (*)(double, double);
    enum class IntForm { None, Abs, Min, Max };

    AnalogMathFunction(std::string_view name, F1 f1) :
        AnalogBuiltin(name, AnalogBuiltinKind::Math), f1(f1), arity(1) {}
    AnalogMathFunction(std::string_view name, F2 f2, IntForm intForm = IntForm::None) :
        AnalogBuiltin(name, AnalogBuiltinKind::Math), f2(f2), arity(2), intForm(intForm) {}
    AnalogMathFunction(std::string_view name, F1 f1, IntForm intForm) :
        AnalogBuiltin(name, AnalogBuiltinKind::Math), f1(f1), arity(1), intForm(intForm) {}

    const Type& checkArguments(const ASTContext& context, const Args& args, SourceRange range,
                               const Expression*) const final {
        auto& comp = context.getCompilation();
        if (!checkArgCount(context, false, args, range, arity, arity) ||
            !checkNumericArgs(context, args)) {
            return comp.getErrorType();
        }

        // abs, min, max of integer arguments are integer (4.3.1).
        if (intForm != IntForm::None &&
            std::ranges::all_of(args, [](auto arg) { return arg->type->isIntegral(); })) {
            return comp.getIntegerType();
        }
        return comp.getRealType();
    }

    ConstantValue eval(EvalContext& context, const Args& args, SourceRange,
                       const CallExpression::SystemCallInfo&) const final {
        SmallVector<ConstantValue, 2> values;
        for (auto arg : args) {
            auto cv = arg->eval(context);
            if (!cv)
                return nullptr;
            values.push_back(std::move(cv));
        }

        if (intForm != IntForm::None &&
            std::ranges::all_of(args, [](auto arg) { return arg->type->isIntegral(); })) {
            SmallVector<int64_t, 2> ints;
            for (auto& v : values) {
                auto i = v.integer().as<int64_t>();
                if (!i)
                    return nullptr;
                ints.push_back(*i);
            }
            int64_t r = 0;
            switch (intForm) {
                case IntForm::Abs:
                    r = ints[0] < 0 ? -ints[0] : ints[0];
                    break;
                case IntForm::Min:
                    r = std::min(ints[0], ints[1]);
                    break;
                case IntForm::Max:
                    r = std::max(ints[0], ints[1]);
                    break;
                case IntForm::None:
                    break;
            }
            return SVInt(32, uint64_t(r), true);
        }

        double a = values[0].convertToReal().real();
        if (arity == 1)
            return real_t(f1(a));
        double b = values[1].convertToReal().real();
        return real_t(f2(a, b));
    }

private:
    F1 f1 = nullptr;
    F2 f2 = nullptr;
    size_t arity;
    IntForm intForm = IntForm::None;
};

static double fmin2(double a, double b) {
    return std::fmin(a, b);
}
static double fmax2(double a, double b) {
    return std::fmax(a, b);
}
static double fabs1(double a) {
    return std::fabs(a);
}
static double ln1(double a) {
    return std::log(a);
}
static double log10_1(double a) {
    return std::log10(a);
}
static double exp1(double a) {
    return std::exp(a);
}
static double sqrt1(double a) {
    return std::sqrt(a);
}
static double floor1(double a) {
    return std::floor(a);
}
static double ceil1(double a) {
    return std::ceil(a);
}
static double ln1p1(double a) {
    return std::log1p(a);
}
static double expm1_1(double a) {
    return std::expm1(a);
}
static double pow2(double a, double b) {
    return std::pow(a, b);
}

// ddt, idt, transition, limexp (VAMS-2023 4.5.5, 4.5.6, 4.5.8, 4.5.13): real-valued analog
// operators with state; analog block only; never constant.
class AnalogOperatorFunction : public AnalogBuiltin {
public:
    AnalogOperatorFunction(std::string_view name, AnalogBuiltinKind kind, size_t minArgs,
                           size_t maxArgs) :
        AnalogBuiltin(name, kind), minArgs(minArgs), maxArgs(maxArgs) {}

    bool allowEmptyArgument(size_t index) const final { return index > 0; }

    const Type& checkArguments(const ASTContext& context, const Args& args, SourceRange range,
                               const Expression*) const final {
        auto& comp = context.getCompilation();
        if (!checkArgCount(context, false, args, range, minArgs, maxArgs) ||
            !checkNumericArgs(context, args) || !requireAnalogBlock(context, name, range)) {
            return comp.getErrorType();
        }
        return comp.getRealType();
    }

    ConstantValue eval(EvalContext& context, const Args&, SourceRange range,
                       const CallExpression::SystemCallInfo&) const final {
        notConst(context, range);
        return nullptr;
    }

private:
    size_t minArgs;
    size_t maxArgs;
};

// analysis("name" {, "name"}) (VAMS-2023 4.6.1): integer, string literal arguments.
class AnalysisFunction : public AnalogBuiltin {
public:
    AnalysisFunction() : AnalogBuiltin("analysis", AnalogBuiltinKind::Analysis) {}

    const Type& checkArguments(const ASTContext& context, const Args& args, SourceRange range,
                               const Expression*) const final {
        auto& comp = context.getCompilation();
        if (!checkArgCount(context, false, args, range, 1, INT32_MAX))
            return comp.getErrorType();

        for (auto arg : args) {
            if (arg->kind != ExpressionKind::StringLiteral) {
                context.addDiag(diag::AnalysisArgNotString, arg->sourceRange);
                return comp.getErrorType();
            }
        }
        return comp.getIntegerType();
    }

    ConstantValue eval(EvalContext& context, const Args&, SourceRange range,
                       const CallExpression::SystemCallInfo&) const final {
        notConst(context, range);
        return nullptr;
    }
};

// $abstime (VAMS-2023 9.10): absolute simulation time in seconds, a real.
class AbstimeFunction : public AnalogBuiltin {
public:
    AbstimeFunction() : AnalogBuiltin("$abstime", AnalogBuiltinKind::Abstime) {}

    const Type& checkArguments(const ASTContext& context, const Args& args, SourceRange range,
                               const Expression*) const final {
        auto& comp = context.getCompilation();
        if (!checkArgCount(context, false, args, range, 0, 0))
            return comp.getErrorType();
        return comp.getRealType();
    }

    ConstantValue eval(EvalContext& context, const Args&, SourceRange range,
                       const CallExpression::SystemCallInfo&) const final {
        notConst(context, range);
        return nullptr;
    }
};

// initial_step, final_step, cross (VAMS-2023 5.10): legal only as an event control, where
// SignalEventControl turns the call into an AnalogEventControl.
class AnalogEventFunction : public AnalogBuiltin {
public:
    AnalogEventFunction(std::string_view name, AnalogBuiltinKind kind) :
        AnalogBuiltin(name, kind) {}

    bool allowEmptyArgument(size_t index) const final {
        return analogKind == AnalogBuiltinKind::Cross && index > 0;
    }

    const Type& checkArguments(const ASTContext& context, const Args& args, SourceRange range,
                               const Expression*) const final {
        auto& comp = context.getCompilation();
        if (!context.flags.has(ASTFlags::EventExpression)) {
            context.addDiag(diag::AnalogEventNotInEventControl, range) << name;
            return comp.getErrorType();
        }

        if (analogKind == AnalogBuiltinKind::Cross) {
            if (!checkArgCount(context, false, args, range, 1, 5) ||
                !checkNumericArgs(context, args)) {
                return comp.getErrorType();
            }
        }
        else {
            for (auto arg : args) {
                if (arg->kind != ExpressionKind::StringLiteral) {
                    context.addDiag(diag::AnalogEventArgNotString, arg->sourceRange) << name;
                    return comp.getErrorType();
                }
            }
        }
        return comp.getBitType();
    }

    ConstantValue eval(EvalContext& context, const Args&, SourceRange range,
                       const CallExpression::SystemCallInfo&) const final {
        notConst(context, range);
        return nullptr;
    }
};

// A reserved Verilog-AMS function whose semantics are P1.
class UnsupportedAnalogFunction : public AnalogBuiltin {
public:
    explicit UnsupportedAnalogFunction(std::string_view name) :
        AnalogBuiltin(name, AnalogBuiltinKind::Unsupported) {}

    bool allowEmptyArgument(size_t) const final { return true; }

    const Type& checkArguments(const ASTContext& context, const Args&, SourceRange range,
                               const Expression*) const final {
        context.addDiag(diag::VamsNotSupported, range) << fmt::format("'{}'", name);
        return context.getCompilation().getErrorType();
    }

    ConstantValue eval(EvalContext&, const Args&, SourceRange,
                       const CallExpression::SystemCallInfo&) const final {
        return nullptr;
    }
};

void Builtins::registerAnalogFuncs() {
    using IF = AnalogMathFunction::IntForm;
    auto add = [this](TokenKind kind, std::shared_ptr<SystemSubroutine> sub) {
        analogBuiltins.emplace(kind, std::move(sub));
    };
    auto math1 = [&](TokenKind kind, std::string_view name, AnalogMathFunction::F1 f) {
        add(kind, std::make_shared<AnalogMathFunction>(name, f));
    };
    auto math2 = [&](TokenKind kind, std::string_view name, AnalogMathFunction::F2 f) {
        add(kind, std::make_shared<AnalogMathFunction>(name, f));
    };

    math1(TokenKind::LnKeyword, "ln", ln1);
    math1(TokenKind::LogKeyword, "log", log10_1);
    math1(TokenKind::ExpKeyword, "exp", exp1);
    math1(TokenKind::SqrtKeyword, "sqrt", sqrt1);
    math1(TokenKind::FloorKeyword, "floor", floor1);
    math1(TokenKind::CeilKeyword, "ceil", ceil1);
    math1(TokenKind::Ln1pKeyword, "ln1p", ln1p1);
    math1(TokenKind::Expm1Keyword, "expm1", expm1_1);
    math1(TokenKind::SinKeyword, "sin", static_cast<double (*)(double)>(std::sin));
    math1(TokenKind::CosKeyword, "cos", static_cast<double (*)(double)>(std::cos));
    math1(TokenKind::TanKeyword, "tan", static_cast<double (*)(double)>(std::tan));
    math1(TokenKind::AsinKeyword, "asin", static_cast<double (*)(double)>(std::asin));
    math1(TokenKind::AcosKeyword, "acos", static_cast<double (*)(double)>(std::acos));
    math1(TokenKind::AtanKeyword, "atan", static_cast<double (*)(double)>(std::atan));
    math1(TokenKind::SinhKeyword, "sinh", static_cast<double (*)(double)>(std::sinh));
    math1(TokenKind::CoshKeyword, "cosh", static_cast<double (*)(double)>(std::cosh));
    math1(TokenKind::TanhKeyword, "tanh", static_cast<double (*)(double)>(std::tanh));
    math1(TokenKind::AsinhKeyword, "asinh", static_cast<double (*)(double)>(std::asinh));
    math1(TokenKind::AcoshKeyword, "acosh", static_cast<double (*)(double)>(std::acosh));
    math1(TokenKind::AtanhKeyword, "atanh", static_cast<double (*)(double)>(std::atanh));
    math2(TokenKind::PowKeyword, "pow", pow2);
    math2(TokenKind::Atan2Keyword, "atan2", static_cast<double (*)(double, double)>(std::atan2));
    math2(TokenKind::HypotKeyword, "hypot", static_cast<double (*)(double, double)>(std::hypot));
    add(TokenKind::AbsKeyword, std::make_shared<AnalogMathFunction>("abs", fabs1, IF::Abs));
    add(TokenKind::MinKeyword, std::make_shared<AnalogMathFunction>("min", fmin2, IF::Min));
    add(TokenKind::MaxKeyword, std::make_shared<AnalogMathFunction>("max", fmax2, IF::Max));

    using AK = AnalogBuiltinKind;
    add(TokenKind::DdtKeyword, std::make_shared<AnalogOperatorFunction>("ddt", AK::Ddt, 1, 2));
    add(TokenKind::IdtKeyword, std::make_shared<AnalogOperatorFunction>("idt", AK::Idt, 1, 4));
    add(TokenKind::TransitionKeyword,
        std::make_shared<AnalogOperatorFunction>("transition", AK::Transition, 1, 5));
    add(TokenKind::LimexpKeyword,
        std::make_shared<AnalogOperatorFunction>("limexp", AK::Limexp, 1, 1));
    add(TokenKind::AnalysisKeyword, std::make_shared<AnalysisFunction>());
    add(TokenKind::InitialStepKeyword,
        std::make_shared<AnalogEventFunction>("initial_step", AK::InitialStep));
    add(TokenKind::FinalStepKeyword,
        std::make_shared<AnalogEventFunction>("final_step", AK::FinalStep));
    add(TokenKind::CrossKeyword, std::make_shared<AnalogEventFunction>("cross", AK::Cross));

    for (auto [kind, name] : {std::pair{TokenKind::DdxKeyword, "ddx"},
                              {TokenKind::IdtmodKeyword, "idtmod"},
                              {TokenKind::AbsdelayKeyword, "absdelay"},
                              {TokenKind::SlewKeyword, "slew"},
                              {TokenKind::LastCrossingKeyword, "last_crossing"},
                              {TokenKind::LaplaceNdKeyword, "laplace_nd"},
                              {TokenKind::LaplaceNpKeyword, "laplace_np"},
                              {TokenKind::LaplaceZdKeyword, "laplace_zd"},
                              {TokenKind::LaplaceZpKeyword, "laplace_zp"},
                              {TokenKind::ZiNdKeyword, "zi_nd"},
                              {TokenKind::ZiNpKeyword, "zi_np"},
                              {TokenKind::ZiZdKeyword, "zi_zd"},
                              {TokenKind::ZiZpKeyword, "zi_zp"},
                              {TokenKind::AcStimKeyword, "ac_stim"},
                              {TokenKind::WhiteNoiseKeyword, "white_noise"},
                              {TokenKind::FlickerNoiseKeyword, "flicker_noise"},
                              {TokenKind::NoiseTableKeyword, "noise_table"},
                              {TokenKind::NoiseTableLogKeyword, "noise_table_log"},
                              {TokenKind::AboveKeyword, "above"},
                              {TokenKind::TimerKeyword, "timer"},
                              {TokenKind::AbsdeltaKeyword, "absdelta"}}) {
        add(kind, std::make_shared<UnsupportedAnalogFunction>(name));
    }

    vamsSystemFunctions.emplace("$abstime", std::make_shared<AbstimeFunction>());
}

} // namespace slang::ast::builtins
