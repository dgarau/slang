//------------------------------------------------------------------------------
// Parser_vams.cpp
// SVMake VAMS P0.1: Verilog-AMS (Accellera VAMS-2023) declarations
//
// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#include "slang/diagnostics/ParserDiags.h"
#include "slang/diagnostics/VamsDiags.h"
#include "slang/parsing/Parser.h"
#include "slang/parsing/Preprocessor.h"

namespace slang::parsing {

using namespace syntax;

static bool isNatureAttributeName(TokenKind kind) {
    switch (kind) {
        case TokenKind::Identifier:
        case TokenKind::AbstolKeyword:
        case TokenKind::AccessKeyword:
        case TokenKind::DdtNatureKeyword:
        case TokenKind::IdtNatureKeyword:
        case TokenKind::UnitsKeyword:
            return true;
        default:
            return false;
    }
}

// nature_attribute ::= nature_attribute_identifier = nature_attribute_expression ;
NatureAttributeSyntax* Parser::parseNatureAttribute() {
    if (!isNatureAttributeName(peek().kind)) {
        addDiag(diag::ExpectedNatureAttribute, peek().location());
        return nullptr;
    }

    auto name = consume();
    auto equals = expect(TokenKind::Equals);
    auto& value = parseExpression();
    auto semi = expect(TokenKind::Semicolon);
    return &factory.natureAttribute(name, equals, value, semi);
}

// nature_declaration ::= nature nature_identifier [ : parent_nature ] [ ; ]
//                        { nature_item } endnature
NatureDeclarationSyntax& Parser::parseNatureDeclaration(AttrList attributes) {
    auto keyword = consume();
    auto name = expect(TokenKind::Identifier);

    NatureParentClauseSyntax* parent = nullptr;
    if (peek(TokenKind::Colon)) {
        auto colon = consume();
        auto parentName = expect(TokenKind::Identifier);
        Token dot, potentialOrFlow;
        if (peek(TokenKind::Dot)) {
            dot = consume();
            if (peek(TokenKind::PotentialKeyword) || peek(TokenKind::FlowKeyword))
                potentialOrFlow = consume();
            else
                potentialOrFlow = expect(TokenKind::PotentialKeyword);
        }
        parent = &factory.natureParentClause(colon, parentName, dot, potentialOrFlow);
    }

    Token semi;
    if (peek(TokenKind::Semicolon))
        semi = consume();

    SmallVector<NatureAttributeSyntax*> items;
    while (!peek(TokenKind::EndnatureKeyword) && !peek(TokenKind::EndOfFile)) {
        auto current = peek();
        if (auto attr = parseNatureAttribute())
            items.push_back(attr);
        if (current == peek())
            skipToken({});
    }

    auto end = expect(TokenKind::EndnatureKeyword);
    return factory.natureDeclaration(attributes, keyword, name, parent, semi, {alloc, items},
                                     end);
}

// discipline_declaration ::= discipline discipline_identifier [ ; ]
//                            { discipline_item } enddiscipline

DisciplineDeclarationSyntax& Parser::parseDisciplineDeclaration(AttrList attributes) {
    auto keyword = consume();
    auto name = expect(TokenKind::Identifier);

    Token semi;
    if (peek(TokenKind::Semicolon))
        semi = consume();

    SmallVector<SyntaxNode*> items;
    while (!peek(TokenKind::EnddisciplineKeyword) && !peek(TokenKind::EndOfFile)) {
        auto current = peek();
        switch (peek().kind) {
            case TokenKind::PotentialKeyword:
            case TokenKind::FlowKeyword: {
                auto which = consume();
                if (peek(TokenKind::Dot)) {
                    // nature_attribute_override ::= potential_or_flow . nature_attribute
                    auto dot = consume();
                    if (auto attr = parseNatureAttribute())
                        items.push_back(&factory.disciplineAttributeOverride(which, dot, *attr));
                }
                else {
                    // nature_binding ::= potential_or_flow nature_identifier ;
                    auto nature = expect(TokenKind::Identifier);
                    auto s = expect(TokenKind::Semicolon);
                    items.push_back(&factory.disciplineNatureBinding(which, nature, s));
                }
                break;
            }
            case TokenKind::DomainKeyword: {
                // discipline_domain_binding ::= domain discrete_or_continuous ;
                auto domain = consume();
                Token value;
                if (peek(TokenKind::DiscreteKeyword) || peek(TokenKind::ContinuousKeyword))
                    value = consume();
                else
                    value = expect(TokenKind::ContinuousKeyword);
                auto s = expect(TokenKind::Semicolon);
                items.push_back(&factory.disciplineDomainBinding(domain, value, s));
                break;
            }
            default:
                addDiag(diag::ExpectedDisciplineItem, peek().location());
                break;
        }
        if (current == peek())
            skipToken({});
    }

    auto end = expect(TokenKind::EnddisciplineKeyword);
    return factory.disciplineDeclaration(attributes, keyword, name, semi,
                                         {alloc, items}, end);
}

// ground [ discipline_identifier ] [ range ] list_of_net_identifiers ;
GroundDeclarationSyntax& Parser::parseGroundDeclaration(AttrList attributes) {
    auto keyword = consume();

    Token discipline;
    if (peek(TokenKind::Identifier) &&
        (peek(1).kind == TokenKind::Identifier || peek(1).kind == TokenKind::OpenBracket)) {
        discipline = consume();
    }

    auto dims = parseDimensionList();

    Token semi;
    auto declarators = parseDeclarators(semi);
    return factory.groundDeclaration(attributes, keyword, discipline, dims, declarators, semi);
}

// branch ( branch_terminal [ , branch_terminal ] ) list_of_branch_identifiers ;
BranchDeclarationSyntax& Parser::parseBranchDeclaration(AttrList attributes) {
    auto keyword = consume();

    // port_branch_declaration ::= branch ( < port_identifier > ) ... is P1.
    if (peek(TokenKind::OpenParenthesis) && peek(1).kind == TokenKind::LessThan) {
        addDiag(diag::VamsNotSupported, peek(1).location()) << "port branches"sv;
        while (!peek(TokenKind::Semicolon) && !peek(TokenKind::EndOfFile) &&
               !peek(TokenKind::EndModuleKeyword)) {
            skipToken({});
        }
        auto semi = expect(TokenKind::Semicolon);
        return factory.branchDeclaration(attributes, keyword, Token(), {}, Token(), {}, semi);
    }

    Token openParen, closeParen;
    SeparatedSyntaxList<ExpressionSyntax> terminals;
    parseList<isPossibleExpressionOrComma, isEndOfParenList>(
        TokenKind::OpenParenthesis, TokenKind::CloseParenthesis, TokenKind::Comma, openParen,
        terminals, closeParen, RequireItems::True, diag::ExpectedExpression,
        [this] { return &parseExpression(); });

    Token semi;
    auto declarators = parseDeclarators(semi);
    return factory.branchDeclaration(attributes, keyword, openParen, terminals, closeParen,
                                     declarators, semi);
}

} // namespace slang::parsing

namespace slang::parsing {

using namespace syntax;

// analog_construct ::= analog analog_statement | analog initial analog_function_statement
AnalogBlockSyntax& Parser::parseAnalogBlock(AttrList attributes) {
    auto keyword = consume();
    Token initial;
    if (peek(TokenKind::InitialKeyword))
        initial = consume();
    auto& statement = parseStatement();
    return factory.analogBlock(attributes, keyword, initial, statement);
}

} // namespace slang::parsing
