//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

#include "JsErrorTranslator.h"

#include <QList>
#include <QRegularExpression>

// Host-side translation of engine error messages into Russian. The engine
// itself never translates: uncaughtException and the error values keep the
// engine's own message text (QJSEngine parity), and the host decides what
// to show the end user by passing a reported message through this function.
//
// The tables are pinned to the vendored quickjs-ng 0.18.0 message formats
// (quickjs.c / libregexp.c) plus this wrapper's own module-loading texts;
// re-verify them whenever the vendored engine is upgraded. quickjs-ng
// emits most runtime messages lower-case and the parser messages
// capitalized - matching is case-insensitive to cover both spellings.
//
// An "ErrorClass: " prefix (the shape Error.prototype.toString produces)
// is split off and kept untranslated; the class name is an identifier.
// The body must match a rule IN FULL: exact rules compare the whole body,
// parametrized rules are anchored (^...$). The previous substring-replace
// approach corrupted longer messages that merely contained a known
// fragment ("invalid regular expression" biting "invalid regular
// expression flags", "duplicate parameter name" biting quickjs-ng's
// "Duplicate parameter name not allowed in this context"). Unrecognized
// messages pass through unchanged.

QString translateJsError(const QString& errorMessage) {
    if (errorMessage.isEmpty())
        return errorMessage;

    // Split "ErrorClass: body" (Error.prototype.toString shape); anything
    // without such a prefix is matched as a whole.
    QString prefix;
    QString body = errorMessage;
    static const QRegularExpression classSplit(QStringLiteral("^(\\w*Error): (.+)$"));
    const QRegularExpressionMatch split = classSplit.match(errorMessage);
    if (split.hasMatch()) {
        prefix = split.captured(1) + QLatin1String(": ");
        body = split.captured(2);
    }

    // Parameterless formats: one case-insensitive whole-body compare.
    struct ExactRule { QString en; QString ru; };
    static const QList<ExactRule> exact = {
        // -- parser: quickjs.c js_parse_error literals --
        {QStringLiteral("invalid number literal"), QStringLiteral("неверный числовой литерал")},
        {QStringLiteral("Invalid or unexpected token"), QStringLiteral("недопустимый или неожиданный токен")},
        {QStringLiteral("Unexpected number"), QStringLiteral("неожиданное число")},
        {QStringLiteral("Unexpected string"), QStringLiteral("неожиданная строка")},
        {QStringLiteral("Unexpected regexp"), QStringLiteral("неожиданное регулярное выражение")},
        {QStringLiteral("Unexpected string template"), QStringLiteral("неожиданный шаблон строки")},
        {QStringLiteral("unexpected end of input"), QStringLiteral("неожиданный конец ввода")},
        {QStringLiteral("unexpected end of string"), QStringLiteral("неожиданный конец строки")},
        {QStringLiteral("unexpected end of comment"), QStringLiteral("неожиданный конец комментария")},
        {QStringLiteral("unexpected end of regexp"), QStringLiteral("неожиданный конец регулярного выражения")},
        {QStringLiteral("unexpected character"), QStringLiteral("неожиданный символ")},
        {QStringLiteral("unexpected data at the end"), QStringLiteral("неожиданные данные в конце")},
        {QStringLiteral("invalid UTF-8 sequence"), QStringLiteral("недопустимая последовательность UTF-8")},
        {QStringLiteral("Octal literals are not allowed in strict mode"), QStringLiteral("восьмеричные литералы не разрешены в строгом режиме")},
        {QStringLiteral("Duplicate parameter name not allowed in this context"), QStringLiteral("дублирующееся имя параметра не разрешено в этом контексте")},
        {QStringLiteral("duplicate __proto__ property name"), QStringLiteral("дублирующееся имя свойства __proto__")},
        {QStringLiteral("duplicate default"), QStringLiteral("дублирующийся default")},
        {QStringLiteral("duplicate label name"), QStringLiteral("дублирующееся имя метки")},
        {QStringLiteral("invalid redefinition of a variable"), QStringLiteral("недопустимое переопределение переменной")},
        {QStringLiteral("invalid redefinition of global identifier"), QStringLiteral("недопустимое переопределение глобального идентификатора")},
        {QStringLiteral("invalid redefinition of global identifier in module code"), QStringLiteral("недопустимое переопределение глобального идентификатора в коде модуля")},
        {QStringLiteral("invalid redefinition of lexical identifier"), QStringLiteral("недопустимое переопределение лексического идентификатора")},
        {QStringLiteral("invalid redefinition of parameter name"), QStringLiteral("недопустимое переопределение имени параметра")},
        {QStringLiteral("break must be inside loop or switch"), QStringLiteral("break должен находиться внутри цикла или switch")},
        {QStringLiteral("continue must be inside loop"), QStringLiteral("continue должен находиться внутри цикла")},
        {QStringLiteral("break/continue label not found"), QStringLiteral("метка break/continue не найдена")},
        {QStringLiteral("return not in a function"), QStringLiteral("return вне функции")},
        {QStringLiteral("return in a static initializer block"), QStringLiteral("return в статическом блоке инициализации")},
        {QStringLiteral("missing initializer for variable"), QStringLiteral("отсутствует инициализатор переменной")},
        {QStringLiteral("missing formal parameter"), QStringLiteral("отсутствует формальный параметр")},
        {QStringLiteral("variable name expected"), QStringLiteral("ожидается имя переменной")},
        {QStringLiteral("identifier expected"), QStringLiteral("ожидается идентификатор")},
        {QStringLiteral("identifier or string expected"), QStringLiteral("ожидается идентификатор или строка")},
        {QStringLiteral("string expected"), QStringLiteral("ожидается строка")},
        {QStringLiteral("function name expected"), QStringLiteral("ожидается имя функции")},
        {QStringLiteral("invalid destructuring target"), QStringLiteral("недопустимая цель деструктуризации")},
        {QStringLiteral("invalid assignment left-hand side"), QStringLiteral("недопустимая левая часть присваивания")},
        {QStringLiteral("invalid assignment syntax"), QStringLiteral("недопустимый синтаксис присваивания")},
        {QStringLiteral("invalid for in/of left hand-side"), QStringLiteral("недопустимая левая часть в for in/of")},
        {QStringLiteral("invalid increment/decrement operand"), QStringLiteral("недопустимый операнд инкремента/декремента")},
        {QStringLiteral("invalid lvalue in strict mode"), QStringLiteral("недопустимое lvalue в строгом режиме")},
        {QStringLiteral("invalid property name"), QStringLiteral("недопустимое имя свойства")},
        {QStringLiteral("invalid method name"), QStringLiteral("недопустимое имя метода")},
        {QStringLiteral("invalid field name"), QStringLiteral("недопустимое имя поля")},
        {QStringLiteral("invalid switch statement"), QStringLiteral("недопустимый оператор switch")},
        {QStringLiteral("invalid keyword: with"), QStringLiteral("недопустимое ключевое слово: with")},
        {QStringLiteral("too complicated destructuring expression"), QStringLiteral("слишком сложное выражение деструктуризации")},
        {QStringLiteral("expecting catch or finally"), QStringLiteral("ожидается catch или finally")},
        {QStringLiteral("expecting 'as'"), QStringLiteral("ожидается 'as'")},
        {QStringLiteral("expecting field name"), QStringLiteral("ожидается имя поля")},
        {QStringLiteral("expecting target"), QStringLiteral("ожидается цель")},
        {QStringLiteral("from clause expected"), QStringLiteral("ожидается предложение from")},
        {QStringLiteral("expected 'of' or 'in' in for control expression"), QStringLiteral("ожидается 'of' или 'in' в управляющем выражении for")},
        {QStringLiteral("class statement requires a name"), QStringLiteral("объявлению класса требуется имя")},
        {QStringLiteral("class declarations can't appear in single-statement context"), QStringLiteral("объявление class не может стоять в контексте одиночного оператора")},
        {QStringLiteral("function declarations can't appear in single-statement context"), QStringLiteral("объявление function не может стоять в контексте одиночного оператора")},
        {QStringLiteral("lexical declarations can't appear in single-statement context"), QStringLiteral("лексические объявления не могут стоять в контексте одиночного оператора")},
        {QStringLiteral("unexpected 'await' keyword"), QStringLiteral("неожиданное ключевое слово 'await'")},
        {QStringLiteral("unexpected 'yield' keyword"), QStringLiteral("неожиданное ключевое слово 'yield'")},
        {QStringLiteral("await in default expression"), QStringLiteral("await в выражении по умолчанию")},
        {QStringLiteral("yield in default expression"), QStringLiteral("yield в выражении по умолчанию")},
        {QStringLiteral("cannot mix ?? with && or ||"), QStringLiteral("нельзя смешивать ?? с && или ||")},
        {QStringLiteral("cannot delete a direct reference in strict mode"), QStringLiteral("нельзя удалять прямую ссылку в строгом режиме")},
        {QStringLiteral("line terminator not allowed after throw"), QStringLiteral("перевод строки не разрешён после throw")},
        {QStringLiteral("Unexpected end of JSON input"), QStringLiteral("неожиданный конец JSON-ввода")},
        {QStringLiteral("assignment rest property must be last"), QStringLiteral("rest-свойство присваивания должно быть последним")},
        {QStringLiteral("rest element must be the last one"), QStringLiteral("rest-элемент должен быть последним")},
        {QStringLiteral("rest element cannot have a default value"), QStringLiteral("rest-элемент не может иметь значения по умолчанию")},
        {QStringLiteral("super() is only valid in a derived class constructor"), QStringLiteral("super() допустим только в конструкторе производного класса")},
        {QStringLiteral("'super' is only valid in a method"), QStringLiteral("'super' допустим только в методе")},
        {QStringLiteral("invalid use of 'super'"), QStringLiteral("недопустимое использование 'super'")},
        {QStringLiteral("private class field is already defined"), QStringLiteral("приватное поле класса уже определено")},
        {QStringLiteral("property constructor appears more than once"), QStringLiteral("свойство constructor встречается более одного раза")},
        {QStringLiteral("import.meta only valid in module code"), QStringLiteral("import.meta допустим только в коде модуля")},
        {QStringLiteral("new.target only allowed within functions"), QStringLiteral("new.target допустим только внутри функций")},
        {QStringLiteral("invalid use of 'import()'"), QStringLiteral("недопустимое использование 'import()'")},
        {QStringLiteral("'undefined' already declared"), QStringLiteral("'undefined' уже объявлен")},
        {QStringLiteral("'arguments' identifier is not allowed in class field initializer"), QStringLiteral("идентификатор 'arguments' не разрешён в инициализаторе поля класса")},
        {QStringLiteral("'yield' is a reserved identifier"), QStringLiteral("'yield' - зарезервированный идентификатор")},
        {QStringLiteral("'let' is not a valid lexical identifier"), QStringLiteral("'let' не является допустимым лексическим идентификатором")},

        // -- runtime: JS_Throw* literals --
        {QStringLiteral("not a function"), QStringLiteral("не является функцией")},
        {QStringLiteral("property is not a function"), QStringLiteral("свойство не является функцией")},
        {QStringLiteral("not a constructor"), QStringLiteral("не является конструктором")},
        {QStringLiteral("not an object"), QStringLiteral("не является объектом")},
        {QStringLiteral("not a generator"), QStringLiteral("не является генератором")},
        {QStringLiteral("cannot invoke a running generator"), QStringLiteral("нельзя вызвать работающий генератор")},
        {QStringLiteral("iterator must return an object"), QStringLiteral("итератор должен возвращать объект")},
        {QStringLiteral("must be called with new"), QStringLiteral("должен вызываться через new")},
        {QStringLiteral("class constructors must be invoked with 'new'"), QStringLiteral("конструкторы классов должны вызываться через 'new'")},
        {QStringLiteral("constructor requires 'new'"), QStringLiteral("конструктор требует 'new'")},
        {QStringLiteral("Cannot convert undefined or null to object"), QStringLiteral("Невозможно преобразовать undefined или null к объекту")},
        {QStringLiteral("cannot convert to object"), QStringLiteral("не удалось преобразовать к объекту")},
        {QStringLiteral("cannot convert to BigInt"), QStringLiteral("не удалось преобразовать к BigInt")},
        {QStringLiteral("invalid BigInt literal"), QStringLiteral("недопустимый литерал BigInt")},
        {QStringLiteral("Cannot mix BigInt and other types, use explicit conversions"), QStringLiteral("Нельзя смешивать BigInt с другими типами, используйте явные преобразования")},
        {QStringLiteral("Cannot assign to read only property"), QStringLiteral("Невозможно присвоить значение свойству, доступному только для чтения")},
        {QStringLiteral("Maximum call stack size exceeded"), QStringLiteral("Превышен максимальный размер стека вызовов")},
        {QStringLiteral("Invalid array length"), QStringLiteral("Недопустимая длина массива")},
        {QStringLiteral("invalid string length"), QStringLiteral("недопустимая длина строки")},
        {QStringLiteral("invalid 'in' operand"), QStringLiteral("недопустимый операнд 'in'")},
        {QStringLiteral("invalid 'instanceof' right operand"), QStringLiteral("недопустимый правый операнд 'instanceof'")},
        {QStringLiteral("Getter must be a function"), QStringLiteral("Геттер должен быть функцией")},
        {QStringLiteral("Setter must be a function"), QStringLiteral("Сеттер должен быть функцией")},
        {QStringLiteral("Object.defineProperties called on non-object"), QStringLiteral("Object.defineProperties вызван не на объекте")},
        {QStringLiteral("object prototype may only be an Object or null"), QStringLiteral("прототипом объекта может быть только объект или null")},
        {QStringLiteral("toString() radix argument must be between 2 and 36"), QStringLiteral("аргумент системы счисления toString() должен быть от 2 до 36")},
        {QStringLiteral("invalid code point"), QStringLiteral("недопустимая кодовая точка")},
        {QStringLiteral("invalid repeat count"), QStringLiteral("недопустимое количество повторений")},
        {QStringLiteral("invalid base64 string"), QStringLiteral("недопустимая строка base64")},
        {QStringLiteral("invalid hex string"), QStringLiteral("недопустимая шестнадцатеричная строка")},
        {QStringLiteral("regexp must have the 'g' flag"), QStringLiteral("регулярное выражение должно иметь флаг 'g'")},
        {QStringLiteral("invalid regular expression"), QStringLiteral("неверное регулярное выражение")},
        {QStringLiteral("invalid regular expression flags"), QStringLiteral("недопустимые флаги регулярного выражения")},
        {QStringLiteral("revoked proxy"), QStringLiteral("proxy отозван")},
        {QStringLiteral("circular reference"), QStringLiteral("циклическая ссылка")},
        {QStringLiteral("circular prototype chain"), QStringLiteral("циклическая цепочка прототипов")},
        {QStringLiteral("ArrayBuffer is detached"), QStringLiteral("ArrayBuffer отсоединён")},
        {QStringLiteral("invalid typed array index"), QStringLiteral("недопустимый индекс типизированного массива")},
        {QStringLiteral("out of memory"), QStringLiteral("недостаточно памяти")},
        {QStringLiteral("out of memory in regexp execution"), QStringLiteral("недостаточно памяти при выполнении регулярного выражения")},
        {QStringLiteral("interrupted"), QStringLiteral("выполнение скрипта прервано")},
        {QStringLiteral("property is not configurable"), QStringLiteral("свойство не настраивается")},
        {QStringLiteral("object is not extensible"), QStringLiteral("объект не расширяем")},

        // -- libregexp (re_parse_error) --
        {QStringLiteral("nothing to repeat"), QStringLiteral("нечего повторять")},
        {QStringLiteral("syntax error"), QStringLiteral("синтаксическая ошибка")},
        {QStringLiteral("invalid escape sequence in regular expression"), QStringLiteral("недопустимая escape-последовательность в регулярном выражении")},
        {QStringLiteral("extraneous characters at the end"), QStringLiteral("лишние символы в конце")},
        {QStringLiteral("stack overflow"), QStringLiteral("переполнение стека")},
    };
    for (const ExactRule& r : exact) {
        if (body.compare(r.en, Qt::CaseInsensitive) == 0)
            return prefix + r.ru;
    }

    // Formats with variable parts: fully anchored regexes; the first full
    // match wins and carries its captured groups into the translation.
    struct ParamRule { QRegularExpression regex; QString replacement; };
    static const QList<ParamRule> param = {
        {QRegularExpression(QStringLiteral(R"(^expecting '([^']+)'$)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(R"(ожидается '\1')")},
        {QRegularExpression(QStringLiteral(R"(^unexpected token: '([^']+)'$)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(R"(неожиданный токен: '\1')")},
        {QRegularExpression(QStringLiteral(R"(^unexpected token '([^']+)'$)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(R"(неожиданный токен '\1')")},
        {QRegularExpression(QStringLiteral(R"(^unexpected token in expression: '([^']+)'$)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(R"(неожиданный токен в выражении: '\1')")},
        {QRegularExpression(QStringLiteral(R"(^Unexpected identifier '([^']+)'$)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(R"(неожиданный идентификатор '\1')")},
        {QRegularExpression(QStringLiteral(R"(^Invalid (\w+) escape sequence$)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(R"(Недопустимая \1 escape-последовательность)")},
        {QRegularExpression(QStringLiteral(R"(^'([^']+)' is a reserved identifier$)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(R"('\1' - зарезервированный идентификатор)")},
        {QRegularExpression(QStringLiteral(R"(^unsupported keyword: (.+)$)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(R"(неподдерживаемое ключевое слово: \1)")},
        {QRegularExpression(QStringLiteral(R"(^(.+) are not allowed in (.+)$)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(R"(\1 не разрешены в \2)")},
        {QRegularExpression(QStringLiteral(R"(^redeclaration of '([^']+)'$)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(R"(повторное объявление '\1')")},
        {QRegularExpression(QStringLiteral(R"(^'([^']+)' is read-only$)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(R"('\1' - только для чтения)")},
        {QRegularExpression(QStringLiteral(R"(^(.+) is not initialized$)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(R"(\1 не инициализирован)")},
        {QRegularExpression(QStringLiteral(R"(^'?([\w.$]+)'? is not defined$)"), QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption), QStringLiteral(R"(\1 не определён)")},
        {QRegularExpression(QStringLiteral(R"(^Cannot read property '([^']+)' of (null|undefined)$)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(R"(Невозможно прочитать свойство '\1' из \2)")},
        {QRegularExpression(QStringLiteral(R"(^Cannot set property '([^']+)' of (null|undefined)$)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(R"(Невозможно установить свойство '\1' для \2)")},
        {QRegularExpression(QStringLiteral(R"(^'?([\w.$]+)'? is not a function$)"), QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption), QStringLiteral(R"(\1 не является функцией)")},
        {QRegularExpression(QStringLiteral(R"(^'?([\w.$]+)'? is not iterable$)"), QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption), QStringLiteral(R"(\1 не является итерируемым)")},
        {QRegularExpression(QStringLiteral(R"(^'?([\w.$]+)'? is not a constructor$)"), QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption), QStringLiteral(R"(\1 не является конструктором)")},
        {QRegularExpression(QStringLiteral(R"(^'?([\w.$]+)'? is not an object$)"), QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption), QStringLiteral(R"(\1 не является объектом)")},
        {QRegularExpression(QStringLiteral(R"(^could not load module filename '([^']+)'$)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(R"(не удалось загрузить файл модуля '\1')")},
        {QRegularExpression(QStringLiteral(R"(^could not load module '([^']+)'$)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(R"(не удалось загрузить модуль '\1')")},
        {QRegularExpression(QStringLiteral(R"(^Could not find export '([^']+)' in module '([^']+)'$)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(R"(Не удалось найти экспорт '\1' в модуле '\2')")},
        {QRegularExpression(QStringLiteral(R"(^circular reference when looking for export '([^']+)' in module '([^']+)'$)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(R"(циклическая ссылка при поиске экспорта '\1' в модуле '\2')")},
        {QRegularExpression(QStringLiteral(R"(^circular import: binding '([^']+)' is not resolvable in module '([^']+)'$)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(R"(циклический импорт: привязка '\1' не разрешается в модуле '\2')")},
        {QRegularExpression(QStringLiteral(R"(^(.+) in JSON at position (\d+) \(line (\d+) column (\d+)\)$)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(R"(\1 в JSON на позиции \2 (строка \3, столбец \4))")},
        {QRegularExpression(QStringLiteral(R"(^too many arguments in function call \(only (\d+) allowed\)$)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(R"(слишком много аргументов в вызове функции (допустимо не более \1))")},
        {QRegularExpression(QStringLiteral(R"(^a declaration in the head of a for-(\w+) loop can't have an initializer$)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(R"(объявление в заголовке цикла for-\1 не может иметь инициализатор)")},
    };
    for (const ParamRule& r : param) {
        QString translated = body;
        translated.replace(r.regex, r.replacement);
        if (translated != body)
            return prefix + translated;
    }

    return errorMessage;
}
