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

void applyAdaLexer(QsciScintilla *e, const QFont &baseFont)
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

    // Base font on every style, then select the Ada lexer and feed keywords.
    const QByteArray fam = baseFont.family().toUtf8();
    for (int s = 0; s <= SCE_ADA_ILLEGAL; ++s) {
        e->SendScintilla(QsciScintilla::SCI_STYLESETFONT, (unsigned long)s, fam.constData());
        set(QsciScintilla::SCI_STYLESETSIZE, (unsigned long)s, baseFont.pointSize());
    }
    set(QsciScintilla::SCI_SETLEXER, QsciScintilla::SCLEX_ADA, 0);
    e->SendScintilla(QsciScintilla::SCI_SETKEYWORDS, 0UL, kAdaKeywords);

    // Colour scheme (light background).
    styleColour(SCE_ADA_DEFAULT,      QColor("#000000"));
    styleColour(SCE_ADA_WORD,         QColor("#0000c0")); styleBold(SCE_ADA_WORD, true);
    styleColour(SCE_ADA_IDENTIFIER,   QColor("#000000"));
    styleColour(SCE_ADA_NUMBER,       QColor("#008000"));
    styleColour(SCE_ADA_DELIMITER,    QColor("#404040"));
    styleColour(SCE_ADA_CHARACTER,    QColor("#a00050"));
    styleColour(SCE_ADA_CHARACTEREOL, QColor("#a00050"));
    styleColour(SCE_ADA_STRING,       QColor("#a00050"));
    styleColour(SCE_ADA_STRINGEOL,    QColor("#a00050"));
    styleColour(SCE_ADA_LABEL,        QColor("#7f0000"));
    styleColour(SCE_ADA_COMMENTLINE,  QColor("#808080"));
    styleColour(SCE_ADA_ILLEGAL,      QColor("#ff0000"));

    e->SendScintilla(QsciScintilla::SCI_COLOURISE, 0UL, -1L);
}
