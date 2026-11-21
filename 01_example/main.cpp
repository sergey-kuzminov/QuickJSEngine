#include <QCoreApplication>
#include <QDebug>
#include <QObject>

// Uncomment DEFINES += USE_QT_JSENGINE in example.pro to build this example
// against the stock Qt QJSEngine instead of QQuickJSEngine.

#ifdef USE_QT_JSENGINE
#include <QJSEngine>
#include <QJSValue>
using EngineClass = QJSEngine;
using ValueClass = QJSValue;
#else
#include "QuickJSEngine.h"
using EngineClass = QQuickJSEngine;
using ValueClass = QQuickJSValue;
#endif

// =================================================================================
// A simple C++ object exposed to JavaScript
// =================================================================================
class Calculator : public QObject {
    Q_OBJECT
public:
    explicit Calculator(QObject *parent = nullptr) : QObject(parent) {}

    // This method can be called from JS
    Q_INVOKABLE int multiply(int a, int b) {
        return a * b;
    }

    // Read from C++ to verify the JS round-trip
    int value() const { return m_value; }

public slots:
    // This slot is invoked from JS; it emits the signal below
    void setValue(int v) {
        if (m_value != v) {
            m_value = v;
            emit valueChanged(v);
        }
    }

signals:
    // JS code can connect to this signal
    void valueChanged(int newValue);

private:
    int m_value = 0;
};

// =================================================================================
// The main example
// =================================================================================
int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);

#ifdef USE_QT_JSENGINE
    qDebug().noquote() << QStringLiteral("=== Testing the QJSEngine (Qt) JavaScript engine ===");
#else
    qDebug().noquote() << QStringLiteral("=== Testing the QQuickJSEngine (quickjs-ng) JavaScript engine ===");
#endif

    int failedSteps = 0;

    // 1. Create the engine and install the console extension (for console.log).
    // Every ValueClass below is declared after `engine` and is therefore
    // destroyed before it: JS values must never outlive their engine.
    EngineClass engine;
    engine.installExtensions(EngineClass::ConsoleExtension);

    // 2. Evaluate a simple arithmetic expression
    ValueClass mathResult = engine.evaluate(QStringLiteral("1 + 2 * 3"));
    if (mathResult.isError()) {
        qWarning().noquote() << QStringLiteral("Math evaluation failed:")
                             << mathResult.toString();
        ++failedSteps;
    } else {
        qDebug().noquote() << QStringLiteral("Result of '1 + 2 * 3':") << mathResult.toInt();
    }

    // 3. Expose a C++ object to JS as the global variable "calc".
    // The engine does not take ownership: `calc` stays a plain stack object.
    Calculator calc;
    ValueClass jsCalc = engine.newQObject(&calc);
    engine.globalObject().setProperty(QStringLiteral("calc"), jsCalc);

    // 4. Call a C++ method from JavaScript
    ValueClass scriptResult = engine.evaluate(QStringLiteral("calc.multiply(10, 20)"));
    if (scriptResult.isError()) {
        qWarning().noquote() << QStringLiteral("Calling the C++ method from JS failed:")
                             << scriptResult.toString();
        ++failedSteps;
    } else {
        qDebug().noquote() << QStringLiteral("Result of calc.multiply(10, 20) from JS:")
                           << scriptResult.toInt();
    }

    // 5. Define a JS function and call it from C++
    ValueClass greetDef = engine.evaluate(QStringLiteral(
        "function greet(name, age) {"
        " return 'Hello, ' + name + '! You are ' + age + ' years old.';"
        "}"));
    if (greetDef.isError()) {
        qWarning().noquote() << QStringLiteral("Defining the JS function failed:")
                             << greetDef.toString();
        ++failedSteps;
    }

    ValueClass greetFunc = engine.globalObject().property(QStringLiteral("greet"));

#ifdef USE_QT_JSENGINE
    // The stock QJSEngine takes a QJSValueList
    QJSValueList args;
    args << QJSValue(QStringLiteral("Sergey")) << QJSValue(45);
#else
    // QQuickJSEngine accepts a plain QVariantList out of the box
    QVariantList args = { QStringLiteral("Sergey"), 45 };
#endif

    if (!greetFunc.isCallable()) {
        qWarning().noquote() << QStringLiteral("greet() is not callable after evaluation");
        ++failedSteps;
    } else {
        ValueClass greetResult = greetFunc.call(args);
        if (greetResult.isError()) {
            qWarning().noquote() << QStringLiteral("Calling the JS function from C++ failed:")
                                 << greetResult.toString();
            ++failedSteps;
        } else {
            qDebug().noquote() << QStringLiteral("Result of calling the JS function from C++:")
                               << greetResult.toString();
        }
    }

    // 6. Signals and slots
    qDebug().noquote() << QStringLiteral("\n--- Signals and slots ---");

    // Connect a JS function to the C++ signal valueChanged
    ValueClass connectResult = engine.evaluate(QStringLiteral(
        "calc.valueChanged.connect(function(newVal) {"
        "   console.log('[JS] valueChanged signal received: ' + newVal);"
        "});"
        ));
    if (connectResult.isError()) {
        qWarning().noquote() << QStringLiteral("Connecting the JS function to the C++ signal failed:")
                             << connectResult.toString();
        ++failedSteps;
    }

    // Invoke the C++ slot from JS; this emits the signal and runs the JS callback
    qDebug().noquote() << QStringLiteral("Calling calc.setValue(42) from JS...");
    ValueClass setResult = engine.evaluate(QStringLiteral("calc.setValue(42);"));
    if (setResult.isError()) {
        qWarning().noquote() << QStringLiteral("Calling the C++ slot from JS failed:")
                             << setResult.toString();
        ++failedSteps;
    } else if (calc.value() != 42) {
        qWarning().noquote() << QStringLiteral("Expected calc.value() == 42 after the JS call, got:")
                             << calc.value();
        ++failedSteps;
    } else {
        qDebug().noquote() << QStringLiteral("C++ state after the JS call: calc.value() ==")
                           << calc.value();
    }

    // 7. Local error handling (isError)
    qDebug().noquote() << QStringLiteral("\n--- Error handling ---");

    // Calling a non-existent method throws a JS TypeError
    ValueClass errorResult = engine.evaluate(QStringLiteral("calc.nonExistentMethod()"));
    if (errorResult.isError()) {
        qWarning().noquote() << QStringLiteral("Locally caught JS error:")
                             << errorResult.toString();
    } else {
        qWarning().noquote() << QStringLiteral("calc.nonExistentMethod() was expected to throw, but did not");
        ++failedSteps;
    }

    // 8. Global error handling via the uncaughtException signal (QQuickJSEngine only)
    qDebug().noquote() << QStringLiteral("\n--- Global error handling ---");

#ifndef USE_QT_JSENGINE
    // QQuickJSEngine reports uncaught exceptions through this signal; the stock
    // QJSEngine has no equivalent, so this part is QuickJS-only.
    QObject::connect(&engine, &QQuickJSEngine::uncaughtException, [](const QString& msg, int line) {
        qWarning().noquote() << QStringLiteral("[signal] Uncaught JS exception at line %1: %2").arg(line).arg(msg);
    });

    // A script with a deliberate uncaught exception
    ValueClass throwResult = engine.evaluate(QStringLiteral(
        "console.log('Running a script that throws...');\n"
        "throw new Error('Deliberate error for the uncaughtException signal');\n"
        ));
    if (!throwResult.isError()) {
        qWarning().noquote() << QStringLiteral("The script was expected to throw, but did not");
        ++failedSteps;
    }
#else
    qDebug().noquote() << QStringLiteral("(Global error handling via a signal is available in QQuickJSEngine only)");
#endif

    if (failedSteps > 0) {
        qWarning().noquote() << QStringLiteral("\n=== Example finished: %1 step(s) failed ===").arg(failedSteps);
        return 1;
    }

    qDebug().noquote() << QStringLiteral("\n=== Example finished successfully ===");
    return 0;
}

#include "main.moc"
