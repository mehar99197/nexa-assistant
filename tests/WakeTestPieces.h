#pragma once

// The pieces of the keyword spotter's bpe.model that the wake tests need (WordPiecesTest, WakePhrasesTest).

#include <QHash>
#include <QString>

namespace nexa::testpieces {

using namespace Qt::StringLiterals;

// Every piece that occurs in WordPiecesTest's phrases and the "Nexa" keyword lines, with its score, plus the
// model's lowest-scored piece (Q), which sets what an unknown character costs.
inline const QHash<QString, float> kPieces{
    {u"S"_s, -2.9737f},  {u"T"_s, -3.5683f},      {u"N"_s, -4.1642f},    {u"E"_s, -4.1970f},    {u"D"_s, -4.2265f},
    {u"Y"_s, -4.5183f},  {u"▁"_s, -4.6242f},      {u"O"_s, -4.6626f},    {u"C"_s, -4.6637f},    {u"R"_s, -4.7055f},
    {u"A"_s, -4.7523f},  {u"U"_s, -4.8031f},      {u"▁S"_s, -4.9738f},   {u"G"_s, -5.0245f},    {u"L"_s, -5.0477f},
    {u"I"_s, -5.1948f},  {u"▁SO"_s, -5.2595f},    {u"OR"_s, -5.2667f},   {u"AR"_s, -5.2759f},   {u"▁HE"_s, -5.3153f},
    {u"K"_s, -5.4402f},  {u"W"_s, -5.4856f},      {u"LL"_s, -5.5720f},   {u"▁T"_s, -5.6011f},   {u"▁W"_s, -5.7251f},
    {u"ON"_s, -5.8591f}, {u"RA"_s, -5.9295f},     {u"SE"_s, -6.0779f},   {u"V"_s, -6.0915f},    {u"NE"_s, -6.1160f},
    {u"H"_s, -6.1748f},  {u"NG"_s, -6.2455f},     {u"LO"_s, -6.2875f},   {u"CK"_s, -6.2890f},   {u"EL"_s, -6.3479f},
    {u"▁O"_s, -6.3548f}, {u"VI"_s, -6.6003f},     {u"Z"_s, -6.6597f},    {u"IS"_s, -6.7041f},   {u"US"_s, -6.7396f},
    {u"X"_s, -6.7431f},  {u"TA"_s, -7.0060f},     {u"HE"_s, -7.1057f},   {u"J"_s, -7.2631f},    {u"▁NE"_s, -7.2809f},
    {u"▁TA"_s, -7.5500f}, {u"▁WORLD"_s, -7.7007f}, {u"▁JA"_s, -7.9239f},  {u"▁NEXT"_s, -8.1131f}, {u"▁OKAY"_s, -8.2925f},
    {u"Q"_s, -9.8577f}};

} // namespace nexa::testpieces
