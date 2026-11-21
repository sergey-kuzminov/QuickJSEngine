//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

#ifndef QUICKJSVALUE_H
#define QUICKJSVALUE_H

#include <QList>
#include <QPointer>
#include <QString>
#include <QStringView>
#include <QVariant>

#include <quickjs.h>

class QQuickJSEngine;

/**
 * @brief The QQuickJSValue class acts as a container for JavaScript values.
 *
 * @warning QQuickJSValue, like QQuickJSEngine, is not thread-safe. All operations
 * on a QQuickJSValue (including destruction) must be performed in the thread
 * that owns the QQuickJSEngine it belongs to. Destroying or accessing a
 * QQuickJSValue from a different thread will trigger an assertion failure in
 * debug builds and leads to undefined behavior in release builds.
 *
 * @warning All QQuickJSValue instances holding references must be destroyed
 * before their QQuickJSEngine. A value that outlives its engine becomes an
 * invalid "zombie": it is inert for its own operations and is rejected when
 * passed to other engines, but the underlying JS reference is leaked
 * (QuickJS debug builds will abort in JS_FreeRuntime).
 */
class QQuickJSValue {
public:
    enum ErrorType {
        GenericError,
        RangeError,
        ReferenceError,
        SyntaxError,
        TypeError,
        URIError
    };

    static constexpr int InlineArgumentCount = 10;

    QQuickJSValue();
    QQuickJSValue(QQuickJSEngine* engine, JSValue val, bool owned);
    ~QQuickJSValue();

    QQuickJSValue(const QQuickJSValue& other);
    QQuickJSValue& operator=(const QQuickJSValue& other);

    QQuickJSValue(QQuickJSValue&& other) noexcept;
    QQuickJSValue& operator=(QQuickJSValue&& other) noexcept;

    [[nodiscard]] JSValue jsVal() const;
    QQuickJSEngine* engine() const;

    bool isValid() const;
    bool isError() const;
    bool isCallable() const;
    bool isObject() const;
    bool isArray() const;
    bool isString() const;
    bool isNumber() const;
    bool isBool() const;
    bool isNull() const;
    bool isUndefined() const;

    [[nodiscard]] static QQuickJSValue fromException(QQuickJSEngine* engine);

    bool isSameContext(const QQuickJSValue& other) const;
    bool areArgsValid(const QList<QQuickJSValue>& args) const;

    bool tryExtractErrorLineNumber(const char* name, QQuickJSValue& outResult) const;

    QQuickJSValue property(const QString& name) const;
    QQuickJSValue property(const QByteArray& name) const;
    QQuickJSValue property(const char* name) const;
    QQuickJSValue property(QStringView name) const;
    QQuickJSValue property(quint32 arrayIndex) const;

    void setProperty(const QString& name, const QQuickJSValue& value);
    void setProperty(const QByteArray& name, const QQuickJSValue& value);
    void setProperty(const char* name, const QQuickJSValue& value);
    void setProperty(QStringView name, const QQuickJSValue& value);

    void setProperty(const QString& name, const QVariant& value);
    void setProperty(const char* name, const QVariant& value);
    void setProperty(QStringView name, const QVariant& value);
    void setProperty(quint32 arrayIndex, const QQuickJSValue& value);
    void setProperty(quint32 arrayIndex, const QVariant& value);

    template<typename T>
    void setProperty(const QString& name, const T& value);
    template<typename T>
    void setProperty(const char* name, const T& value);
    template<typename T>
    void setProperty(quint32 arrayIndex, const T& value);

    bool hasProperty(const QString& name) const;
    bool hasProperty(const QByteArray& name) const;
    bool hasProperty(const char* name) const;
    bool hasProperty(QStringView name) const;
    bool deleteProperty(const QString& name);
    bool deleteProperty(const QByteArray& name);
    bool deleteProperty(const char* name);
    bool deleteProperty(QStringView name);

    int toInt() const;
    bool toBool() const;
    double toDouble() const;
    QString toString() const;
    QVariant toVariant() const;

    QQuickJSValue call(const QVariantList& args = {}, const QQuickJSValue& thisObj = QQuickJSValue());
    QQuickJSValue call(const QList<QQuickJSValue>& args, const QQuickJSValue& thisObj = QQuickJSValue());

    QQuickJSValue construct(const QVariantList& args = {});
    QQuickJSValue callAsConstructor(const QList<QQuickJSValue>& args);

    bool strictlyEquals(const QQuickJSValue& other) const;
    bool instanceOf(const QQuickJSValue& other) const;

private:
    friend class QQuickJSEngine;

    static QQuickJSValue executeJsCall(QQuickJSEngine* engine, JSContext* ctx, JSValueConst funcVal,
                                       JSValueConst* jsArgs, int argc,
                                       const QQuickJSValue& thisObj, bool isConstructor);

    QPointer<QQuickJSEngine> m_engine;
    JSValue m_val;
    bool m_owned;
    bool m_isError;

    inline JSContext* ctx() const;

    void ensureEngineThread() const;
};

using QQuickJSValueList = QList<QQuickJSValue>;

Q_DECLARE_METATYPE(QQuickJSValue)

#endif // QUICKJSVALUE_H
