// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

#include "util/xmlhighlighter.h"

#include <QColor>
#include <QRegularExpression>
#include <QTextDocument>

XmlSyntaxHighlighter::XmlSyntaxHighlighter(QTextDocument *document)
    : QSyntaxHighlighter(document)
{
    m_tagFormat.setForeground(QColor(QStringLiteral("#2f7fd1")));
    m_attributeFormat.setForeground(QColor(QStringLiteral("#d28b26")));
    m_valueFormat.setForeground(QColor(QStringLiteral("#4d9a5b")));
    m_commentFormat.setForeground(QColor(QStringLiteral("#7f8c8d")));
    m_declarationFormat.setForeground(QColor(QStringLiteral("#9b59b6")));
    m_declarationFormat.setFontItalic(true);
}

void XmlSyntaxHighlighter::highlightBlock(const QString &text)
{
    // Comments are highlighted first and take precedence over tags.
    const QRegularExpression comment(QStringLiteral("<!--[\\s\\S]*?-->"));
    auto it = comment.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        setFormat(match.capturedStart(), match.capturedLength(), m_commentFormat);
    }

    const QRegularExpression declaration(QStringLiteral("<\\?[^>]*\\?>|<![^>]*>"));
    it = declaration.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        setFormat(match.capturedStart(), match.capturedLength(), m_declarationFormat);
    }

    const QRegularExpression tag(QStringLiteral("</?[A-Za-z_][\\w:.-]*"));
    it = tag.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        setFormat(match.capturedStart(), match.capturedLength(), m_tagFormat);
    }

    const QRegularExpression attribute(QStringLiteral("[A-Za-z_][\\w:.-]*(?=\\s*=)"));
    it = attribute.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        setFormat(match.capturedStart(), match.capturedLength(), m_attributeFormat);
    }

    const QRegularExpression value(QStringLiteral("\"[^\"]*\"|'[^']*'"));
    it = value.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        setFormat(match.capturedStart(), match.capturedLength(), m_valueFormat);
    }
}

XmlHighlighter::XmlHighlighter(QObject *parent)
    : QObject(parent)
{
}

QQuickTextDocument *XmlHighlighter::textDocument() const
{
    return m_textDocument;
}

void XmlHighlighter::setTextDocument(QQuickTextDocument *document)
{
    if (m_textDocument == document) {
        return;
    }
    if (m_highlighter) {
        delete m_highlighter;
        m_highlighter = nullptr;
    }
    m_textDocument = document;
    if (m_textDocument) {
        m_highlighter = new XmlSyntaxHighlighter(m_textDocument->textDocument());
        m_highlighter->rehighlight();
    }
    Q_EMIT textDocumentChanged();
}
