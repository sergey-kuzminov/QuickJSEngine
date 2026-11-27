//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

#ifndef QUICKJSCONVERT_H
#define QUICKJSCONVERT_H

#include <QVarLengthArray>
#include <QVariant>

#include <quickjs.h>

#include <limits>
#include <ranges>

#include "QuickJSValue.h"

class QQuickJSEngine;

// Cycle detection for JS -> QVariant conversion. At any moment the stack
// holds only the path from the root to the current object, and the depth is
// bounded by MaxVariantConversionDepth, so a linear scan of a stack-allocated
// buffer beats a QSet node per object on every round-trip.
class JSVisitedStack {
public:
    bool contains(const void* p) const {
        return std::ranges::find(m_path, p) != m_path.end();
    }
    void push(void* p) { m_path.append(p); }
    void pop() { m_path.removeLast(); }
private:
    QVarLengthArray<void*, 16> m_path;
};

class QuickJSConvert {
public:
    static QString jsToQString(JSContext* ctx, JSValueConst val);

    // Atom -> property name via the engine-wide cache (m_atomNameCache), shared
    // by all proxy/conversion sites. Returns a null QString only when the atom
    // cannot be rendered (OOM); an empty name is a valid result. Returned by
    // value: the cache still dedupes the rendering, and callers may hold the
    // name across further JS runs without a "valid until the next call"
    // contract to keep in mind.
    static QString atomName(QQuickJSEngine* engine, JSContext* ctx, JSAtom atom);

    static int extractExceptionLine(JSContext* ctx, JSValueConst exc);
    static int parseLineNumberFromStack(const QString& stack);

    static QVariant variantFromJSObject(QQuickJSEngine* engine, JSContext* ctx, JSValueConst val,
                                        JSClassID qobjectClassID, JSClassID objectClassID,
                                        JSVisitedStack& visited, int depth,
                                        QQuickJSValue::ObjectConversionBehavior behavior);
    static JSAtom cachedStringAtom(QQuickJSEngine* engine, JSContext* ctx, const QString& key);
    static JSValue mapToJSObject(QQuickJSEngine* engine, JSContext* ctx, const QVariantMap* mapPtr, int depth);
    static JSValue hashToJSObject(QQuickJSEngine* engine, JSContext* ctx, const QVariantHash* hashPtr, int depth);
    static JSValue listToJSArray(QQuickJSEngine* engine, JSContext* ctx, const QVariantList* listPtr, int depth);
    static JSValue stringListToJSArray(QQuickJSEngine* engine, JSContext* ctx, const QStringList* listPtr);

    // Scalar fast paths shared by toJSValueImpl() and the metacall sites
    // (property/method returns, signal arguments). They must stay
    // behaviorally identical to the generic QVariant-based conversion.
    // int64ToJS()/uint64ToJS() are defined inline: they sit on the hot path
    // of every method return and signal argument conversion.
    static inline JSValue int64ToJS(JSContext* ctx, qint64 v) {
        if (v >= std::numeric_limits<int32_t>::min() && v <= std::numeric_limits<int32_t>::max()) {
            return JS_NewInt32(ctx, static_cast<int32_t>(v));
        }
        if (v >= -9007199254740991LL && v <= 9007199254740991LL) {
            return JS_NewFloat64(ctx, static_cast<double>(v));
        }
        return JS_NewBigInt64(ctx, v);
    }
    static inline JSValue uint64ToJS(JSContext* ctx, quint64 v) {
        if (v <= static_cast<quint64>(std::numeric_limits<int32_t>::max())) {
            return JS_NewInt32(ctx, static_cast<int32_t>(v));
        }
        if (v <= 9007199254740991ULL) {
            return JS_NewFloat64(ctx, static_cast<double>(v));
        }
        return JS_NewBigUint64(ctx, v);
    }
    static JSValue dateTimeToJS(JSContext* ctx, const QDateTime& dt);
    static JSValue dateToJS(JSContext* ctx, const QDate& d);
    static QDate dateTimeToDate(const QDateTime& dt);
    static JSValue timeToJS(JSContext* ctx, const QTime& t);
    static JSValue byteArrayToJS(JSContext* ctx, const QByteArray& ba);

    static int adjustEnumType(int type);
};

#endif // QUICKJSCONVERT_H
