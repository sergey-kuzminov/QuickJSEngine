//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

#include "QuickJSValueIterator.h"

#include "QuickJSEngine_p.h"

QQuickJSValueIterator::QQuickJSValueIterator(const QQuickJSValue& value)
    : m_object(value)
{
    reset(value);
}

QQuickJSValueIterator::~QQuickJSValueIterator() {
    reset(QQuickJSValue());
}

bool QQuickJSValueIterator::hasNext() const {
    return m_count != 0 && m_current + 1 < static_cast<qint64>(m_count);
}

bool QQuickJSValueIterator::next() {
    if (!hasNext()) return false;
    ++m_current;
    return true;
}

QString QQuickJSValueIterator::name() const {
    JSContext* c = ctx();
    if (!c || m_current < 0 || m_current >= static_cast<qint64>(m_count)) return QString();
    m_object.ensureEngineThread();
    const QString name = QuickJSConvert::atomName(m_object.engine(), c, m_props[m_current].atom);
    if (name.isNull()) return QString();
    return name;
}

QQuickJSValue QQuickJSValueIterator::value() const {
    JSContext* c = ctx();
    if (!c || m_current < 0 || m_current >= static_cast<qint64>(m_count)) return QQuickJSValue();
    m_object.ensureEngineThread();
    // The read can run a getter (user JS): entry bookkeeping applies, like
    // the other property reads.
    JsEntryScope entryScope(m_object.engine());
    JSValue val = JS_GetProperty(c, m_object.jsVal(), m_props[m_current].atom);
    if (JS_IsException(val)) {
        // A getter threw: mirror QJSValueIterator, which swallows the
        // exception and reports an invalid value.
        clearPendingException(c);
        return QQuickJSValue();
    }
    return QQuickJSValue(m_object.engine(), val, true);
}

QQuickJSValueIterator& QQuickJSValueIterator::operator=(const QQuickJSValue& value) {
    // Release the previous enumeration while m_object still references the
    // engine that allocated m_props: reset() derives its context from
    // m_object, and the atom array of one runtime must not be freed through
    // another one (a cross-runtime JS_FreePropertyEnum corrupts the heap).
    reset(QQuickJSValue());
    m_object = value;
    reset(value);
    return *this;
}

void QQuickJSValueIterator::reset(const QQuickJSValue& value) {
    JSContext* c = ctx();
    if (c && m_props) {
        JS_FreePropertyEnum(c, m_props, m_count);
    }
    m_props = nullptr;
    m_count = 0;
    m_current = -1;

    // Anything that is not an object (an invalid value, a primitive, a
    // symbol) yields an exhausted iterator, like QJSValueIterator.
    if (!c || !value.isValid() || !JS_IsObject(value.jsVal())) return;

    // JS_GPN_STRING_MASK: own string-keyed properties, enumerability
    // disregarded - the closest match to V4's ownPropertyKeys()-based
    // QJSValueIterator (which also skips symbols but keeps "length" on arrays).
    //
    // For a Proxy the enumeration runs the ownKeys trap - user JS - so the
    // entry bookkeeping applies like for value(): a stale deadline from a
    // previous run must not abort the trap poll with a phantom interrupt
    // (which would surface as a silently exhausted iterator).
    JsEntryScope entryScope(value.engine());
    if (JS_GetOwnPropertyNames(c, &m_props, &m_count, value.jsVal(), JS_GPN_STRING_MASK) < 0) {
        m_props = nullptr;
        m_count = 0;
        clearPendingException(c);
    }
}

JSContext* QQuickJSValueIterator::ctx() const {
    QQuickJSEngine* engine = m_object.engine();
    if (!engine) return nullptr;
    return engine->m_ctx;
}
