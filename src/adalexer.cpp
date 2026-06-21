#include "adalexer.h"

#include <Qsci/qsciscintilla.h>
#include <QColor>

// Scintilla Ada lexer style ids (from Scintilla's SciLexer.h; not re-exported
// by QScintilla's headers, so defined here against their stable values).
namespace {
enum {
    SCE_ADA_DEFAULT      = 0,
    SCE_ADA_WORD         = 1,
    SCE_ADA_IDENTIFIER   = 2,
    SCE_ADA_NUMBER       = 3,
    SCE_ADA_DELIMITER    = 4,
    SCE_ADA_CHARACTER    = 5,
    SCE_ADA_CHARACTEREOL = 6,
    SCE_ADA_STRING       = 7,
    SCE_ADA_STRINGEOL    = 8,
    SCE_ADA_LABEL        = 9,
    SCE_ADA_COMMENTLINE  = 10,
    SCE_ADA_ILLEGAL      = 11,
};

// Scintilla encodes colours as 0x00BBGGRR.
long sciColour(const QColor &c)
{
    return c.red() | (c.green() << 8) | (c.blue() << 16);
}

// Ada 95 keyword set (from SETEdit) + Ada 2005 (interface/overriding/
// synchronized) + Ada 2022 (parallel). Single space-separated, lower case.
const char *kAdaKeywords =
    "abort abs abstract accept access aliased all and array at begin body case "
    "constant declare delay delta digits do else elsif end entry exception exit "
    "for function generic goto if in interface is limited loop mod new not null "
    "of or others out overriding package parallel pragma private protected raise "
    "range record rem renames requeue return reverse select separate some subtype "
    "synchronized tagged task terminate then type until use when while with xor";
}

void applyAdaLexer(QsciScintilla *e, const QFont &baseFont, bool dark)
{
    auto set = [&](unsigned int msg, unsigned long w, long l) {
        e->SendScintilla(msg, w, l);
    };
    auto styleColour = [&](int style, const QColor &c) {
        e->SendScintilla(QsciScintilla::SCI_STYLESETFORE, (unsigned long)style, sciColour(c));
    };
    auto styleBold = [&](int style, bool on) {
        set(QsciScintilla::SCI_STYLESETBOLD, (unsigned long)style, on ? 1 : 0);
    };

    // Background (paper) and default text for the whole control. Seed the
    // template style 32 (STYLE_DEFAULT) with the font + theme colours, then
    // SCI_STYLECLEARALL copies it to every style so the paper is uniform; the
    // per-syntax foregrounds below override just the text colour.
    const QColor bg = dark ? QColor("#1e1e1e") : QColor("#ffffff");
    const QColor fg = dark ? QColor("#d4d4d4") : QColor("#000000");
    const QByteArray fam = baseFont.family().toUtf8();
    e->SendScintilla(QsciScintilla::SCI_STYLESETFONT,
                     (unsigned long)QsciScintilla::STYLE_DEFAULT, fam.constData());
    set(QsciScintilla::SCI_STYLESETSIZE, QsciScintilla::STYLE_DEFAULT, baseFont.pointSize());
    set(QsciScintilla::SCI_STYLESETFORE, QsciScintilla::STYLE_DEFAULT, sciColour(fg));
    set(QsciScintilla::SCI_STYLESETBACK, QsciScintilla::STYLE_DEFAULT, sciColour(bg));
    set(QsciScintilla::SCI_STYLECLEARALL, 0, 0);

    set(QsciScintilla::SCI_SETLEXER, QsciScintilla::SCLEX_ADA, 0);
    e->SendScintilla(QsciScintilla::SCI_SETKEYWORDS, 0UL, kAdaKeywords);

    // Syntax foreground palette: a light scheme, or a dark (VS Code-like) one.
    const QColor cWord    = dark ? QColor("#569cd6") : QColor("#0000c0");
    const QColor cNumber  = dark ? QColor("#b5cea8") : QColor("#008000");
    const QColor cDelim   = dark ? QColor("#d4d4d4") : QColor("#404040");
    const QColor cString  = dark ? QColor("#ce9178") : QColor("#a00050");
    const QColor cLabel   = dark ? QColor("#c586c0") : QColor("#7f0000");
    const QColor cComment = dark ? QColor("#6a9955") : QColor("#808080");
    const QColor cIllegal = dark ? QColor("#f44747") : QColor("#ff0000");

    styleColour(SCE_ADA_DEFAULT,      fg);
    styleColour(SCE_ADA_WORD,         cWord); styleBold(SCE_ADA_WORD, true);
    styleColour(SCE_ADA_IDENTIFIER,   fg);
    styleColour(SCE_ADA_NUMBER,       cNumber);
    styleColour(SCE_ADA_DELIMITER,    cDelim);
    styleColour(SCE_ADA_CHARACTER,    cString);
    styleColour(SCE_ADA_CHARACTEREOL, cString);
    styleColour(SCE_ADA_STRING,       cString);
    styleColour(SCE_ADA_STRINGEOL,    cString);
    styleColour(SCE_ADA_LABEL,        cLabel);
    styleColour(SCE_ADA_COMMENTLINE,  cComment);
    styleColour(SCE_ADA_ILLEGAL,      cIllegal);

    e->SendScintilla(QsciScintilla::SCI_COLOURISE, 0UL, -1L);
}
