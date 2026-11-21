//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

#ifndef JSERRORTRANSLATOR_H
#define JSERRORTRANSLATOR_H

#include <QString>
#include <QRegularExpression>

struct TranslationRule {
    QRegularExpression regex;
    QString replacement;
};

QString translateJsError(const QString& errorMessage);

#endif // JSERRORTRANSLATOR_H
