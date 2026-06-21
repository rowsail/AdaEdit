#pragma once
#include <QFont>

class QsciScintilla;

// Configure a QsciScintilla widget to highlight Ada.
//
// QScintilla 2.14 ships no high-level QsciLexerAda, but the underlying
// Scintilla engine has a native Ada lexer (SCLEX_ADA). We select it directly
// and feed it the keyword set extracted from SETEdit's syntaxhl.shl plus the
// Ada 2005/2022 reserved words (see docs/ada_keywords.txt).
//
// When `dark` is true a dark editor background with a matching syntax palette
// is applied; otherwise the original light scheme is used.
void applyAdaLexer(QsciScintilla *editor, const QFont &baseFont, bool dark = false);
