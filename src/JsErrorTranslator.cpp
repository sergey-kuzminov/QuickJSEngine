//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

#include "JsErrorTranslator.h"

QString translateJsError(const QString& errorMessage) {
    QString translated = errorMessage;

    static const QList<TranslationRule> rules = {
        {QRegularExpression(QStringLiteral("\\binvalid number literal\\b"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("неверный числовой литерал")},
        {QRegularExpression(QStringLiteral("\\binvalid unicode escape sequence\\b"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("неверная unicode escape-последовательность")},
        {QRegularExpression(QStringLiteral("\\binvalid regular expression\\b"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("неверное регулярное выражение")},
        {QRegularExpression(QStringLiteral(R"(unexpected token in expression: '?([^']+)'?)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("неожиданный токен в выражении: '\\1'")},
        {QRegularExpression(QStringLiteral("\\bunexpected end of input\\b"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("неожиданный конец ввода")},
        {QRegularExpression(QStringLiteral(R"(unexpected token '?([^']+)'?)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("неожиданный токен '\\1'")},
        {QRegularExpression(QStringLiteral(R"(expected token '?([^']+)'?)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("ожидается токен '\\1'")},
        {QRegularExpression(QStringLiteral(R"(missing (?:argument|argument list) after '?([^']+)'?)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("отсутствует аргумент после '\\1'")},
        {QRegularExpression(QStringLiteral(R"(duplicate parameter name '?([^']+)'?)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("дублирующееся имя параметра '\\1'")},
        {QRegularExpression(QStringLiteral("\\billegal continue statement\\b"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("недопустимый оператор continue")},
        {QRegularExpression(QStringLiteral("\\billegal break statement\\b"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("недопустимый оператор break")},
        {QRegularExpression(QStringLiteral("\\bcircular dependency\\b"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("циклическая зависимость")},
        {QRegularExpression(QStringLiteral(R"('?([\w.$]+)'? is not defined)"), QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption), QStringLiteral("\\1 не определён")},
        {QRegularExpression(QStringLiteral(R"(Cannot read property '?([^']+)'? of (null|undefined))"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("Невозможно прочитать свойство '\\1' из \\2")},
        {QRegularExpression(QStringLiteral(R"(Cannot set property '?([^']+)'? of (null|undefined))"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("Невозможно установить свойство '\\1' для \\2")},
        {QRegularExpression(QStringLiteral(R"('?([\w.$]+)'? is not a function)"), QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption), QStringLiteral("\\1 не является функцией")},
        {QRegularExpression(QStringLiteral(R"('?([\w.$]+)'? is not iterable)"), QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption), QStringLiteral("\\1 не является итерируемым объектом")},
        {QRegularExpression(QStringLiteral(R"('?([\w.$]+)'? is not a constructor)"), QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption), QStringLiteral("\\1 не является конструктором")},
        {QRegularExpression(QStringLiteral(R"('?([\w.$]+)'? is not an object)"), QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption), QStringLiteral("\\1 не является объектом")},
        {QRegularExpression(QStringLiteral("Maximum call stack size exceeded"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("Превышен максимальный размер стека вызовов")},
        {QRegularExpression(QStringLiteral("Invalid array length"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("Недопустимая длина массива")},
        {QRegularExpression(QStringLiteral("\\binterrupted\\b"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("Выполнение скрипта прервано")},
        {QRegularExpression(QStringLiteral("\\bout of memory\\b"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("Недостаточно памяти")},
        {QRegularExpression(QStringLiteral("property is not configurable"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("свойство не настраивается")},
        {QRegularExpression(QStringLiteral("object is not extensible"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("объект не расширяем")},
        {QRegularExpression(QStringLiteral("could not clone object"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("не удалось клонировать объект")}
    };

    for (const auto& rule : rules) {
        translated.replace(rule.regex, rule.replacement);
    }

    return translated;
}
