//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

#ifndef QUICKJSVALUEITERATOR_H
#define QUICKJSVALUEITERATOR_H

#include "QuickJSValue.h"

/**
 * @brief The QQuickJSValueIterator class provides a QJSValueIterator-compatible
 * iterator over the own properties of a script value.
 *
 * Iteration covers only the object's own properties; the prototype chain is
 * not followed and symbol-keyed properties are skipped, like QJSValueIterator.
 * Non-enumerable own properties (an array's "length", for example) are
 * included. The property value is fetched lazily by value(): a getter runs at
 * that point, and a throwing getter yields an invalid value instead of
 * propagating the exception.
 *
 * @warning Like QQuickJSValue itself, the iterator is not thread-safe: all
 * operations must run in the thread that owns the engine.
 */
class QQuickJSValueIterator {
public:
    explicit QQuickJSValueIterator(const QQuickJSValue& value);
    ~QQuickJSValueIterator();

    QQuickJSValueIterator(const QQuickJSValueIterator&) = delete;
    QQuickJSValueIterator& operator=(const QQuickJSValueIterator&) = delete;

    bool hasNext() const;
    bool next();

    QString name() const;
    QQuickJSValue value() const;

    QQuickJSValueIterator& operator=(const QQuickJSValue& value);

private:
    void reset(const QQuickJSValue& value);
    JSContext* ctx() const;

    QQuickJSValue m_object;
    JSPropertyEnum* m_props = nullptr;
    uint32_t m_count = 0;
    qint64 m_current = -1;
};

#endif // QUICKJSVALUEITERATOR_H
