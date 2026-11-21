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

#include <ranges>

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
    // cannot be rendered (OOM); an empty name is a valid result.
    // The reference stays valid until the next atomName() call on this engine:
    // above the cache capacity the name lives in a single engine-owned fallback
    // slot, and a miss inserts into the cache. Do not run JS or trigger other
    // atom-name lookups while holding it.
    static const QString& atomName(QQuickJSEngine* engine, JSContext* ctx, JSAtom atom);

    static int extractExceptionLine(JSContext* ctx, JSValueConst exc);
    static int parseLineNumberFromStack(const QString& stack);

    static QVariant variantFromJSObject(QQuickJSEngine* engine, JSContext* ctx, JSValueConst val,
                                        JSClassID qobjectClassID, JSClassID objectClassID,
                                        JSVisitedStack& visited, int depth);
    static JSAtom cachedStringAtom(QQuickJSEngine* engine, JSContext* ctx, const QString& key);
    static JSValue mapToJSObject(QQuickJSEngine* engine, JSContext* ctx, const QVariantMap* mapPtr, int depth);
    static JSValue hashToJSObject(QQuickJSEngine* engine, JSContext* ctx, const QVariantHash* hashPtr, int depth);
    static JSValue listToJSArray(QQuickJSEngine* engine, JSContext* ctx, const QVariantList* listPtr, int depth);
    static JSValue stringListToJSArray(QQuickJSEngine* engine, JSContext* ctx, const QStringList* listPtr);

    static int adjustEnumType(int type);
};

#endif // QUICKJSCONVERT_H
