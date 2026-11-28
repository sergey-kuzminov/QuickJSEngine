//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

#ifndef JSERRORTRANSLATOR_H
#define JSERRORTRANSLATOR_H

#include <QString>

// Host-side translation of an engine error message into Russian; the table
// and the matching rules live in JsErrorTranslator.cpp, the contract is
// documented in the README. The engine itself never translates: messages
// the tables do not recognize (and every message, when the host does not
// call this) pass through unchanged.
QString translateJsError(const QString& errorMessage);

#endif // JSERRORTRANSLATOR_H
