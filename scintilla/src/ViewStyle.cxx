// Scintilla source code edit control
/** @file ViewStyle.cxx
 ** Store information on how the document is to be viewed.
 **/
// Copyright 1998-2003 by Neil Hodgson <neilh@scintilla.org>
// The License.txt file describes the conditions under which this software may be distributed.

#include <cstddef>
#include <cassert>
#include <cstring>

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <algorithm>
#include <memory>

#include "Debugging.h"
#include "Geometry.h"
#include "Platform.h"

#include "Scintilla.h"
#include "Position.h"
#include "UniqueString.h"
#include "Indicator.h"
#include "XPM.h"
#include "LineMarker.h"
#include "Style.h"
#include "ViewStyle.h"

using namespace Scintilla;

MarginStyle::MarginStyle(int style_, int width_, MarkerMask mask_) noexcept :
	style(style_), width(width_), mask(mask_), sensitive(false), cursor(SC_CURSORREVERSEARROW) {
}

void FontRealised::Realise(Surface &surface, int zoomLevel, int technology, const FontSpecification &fs, const char *localeName) {
	PLATFORM_ASSERT(fs.fontName);
	sizeZoomed = GetFontSizeZoomed(fs.size, zoomLevel);
	const float deviceHeight = static_cast<float>(surface.DeviceHeightFont(sizeZoomed));
	const FontParameters fp(fs.fontName, deviceHeight / SC_FONT_SIZE_MULTIPLIER, fs.weight,
		fs.italic, fs.extraFontFlag, technology, fs.characterSet, localeName);
	font = Font::Allocate(fp);

	ascent = static_cast<unsigned int>(surface.Ascent(font.get()));
	descent = static_cast<unsigned int>(surface.Descent(font.get()));
	capitalHeight = surface.Ascent(font.get()) - surface.InternalLeading(font.get());
	aveCharWidth = surface.AverageCharWidth(font.get());
	spaceWidth = surface.WidthText(font.get(), " ");
}

ViewStyle::ViewStyle() : markers(MARKER_MAX + 1), indicators(INDICATOR_MAX + 1) {
	Init();
}

// Copy constructor only called when printing copies the screen ViewStyle so it can be
// modified for printing styles.
ViewStyle::ViewStyle(const ViewStyle &source) : markers(MARKER_MAX + 1), indicators(INDICATOR_MAX + 1) {
	Init(source.styles.size());
	styles = source.styles;
	for (size_t sty = 0; sty < source.styles.size(); sty++) {
		// Can't just copy fontName as its lifetime is relative to its owning ViewStyle
		styles[sty].fontName = fontNames.Save(source.styles[sty].fontName);
	}
	nextExtendedStyle = source.nextExtendedStyle;
	markers = source.markers;
	CalcLargestMarkerHeight();

	indicators = source.indicators;

	indicatorsDynamic = source.indicatorsDynamic;
	indicatorsSetFore = source.indicatorsSetFore;

	selection = source.selection;

	foldmarginColour = source.foldmarginColour;
	foldmarginHighlightColour = source.foldmarginHighlightColour;

	hotspotColours = source.hotspotColours;
	hotspotUnderline = source.hotspotUnderline;

	whitespaceColours = source.whitespaceColours;
	controlCharSymbol = source.controlCharSymbol;
	controlCharWidth = source.controlCharWidth;
	selbar = source.selbar;
	selbarlight = source.selbarlight;
	caret = source.caret;
	caretLine = source.caretLine;
	someStylesProtected = false;
	someStylesForceCase = false;
	leftMarginWidth = source.leftMarginWidth;
	rightMarginWidth = source.rightMarginWidth;
	ms = source.ms;
	maskInLine = source.maskInLine;
	maskDrawInText = source.maskDrawInText;
	fixedColumnWidth = source.fixedColumnWidth;
	marginInside = source.marginInside;
	textStart = source.textStart;
	zoomLevel = source.zoomLevel;
	viewWhitespace = source.viewWhitespace;
	tabDrawMode = source.tabDrawMode;
	whitespaceSize = source.whitespaceSize;
	viewIndentationGuides = source.viewIndentationGuides;
	viewEOL = source.viewEOL;
	extraFontFlag = source.extraFontFlag;
	extraAscent = source.extraAscent;
	extraDescent = source.extraDescent;
	marginStyleOffset = source.marginStyleOffset;
	annotationVisible = source.annotationVisible;
	annotationStyleOffset = source.annotationStyleOffset;
	eolAnnotationVisible = source.eolAnnotationVisible;
	eolAnnotationStyleOffset = source.eolAnnotationStyleOffset;
	braceHighlightIndicatorSet = source.braceHighlightIndicatorSet;
	braceHighlightIndicator = source.braceHighlightIndicator;
	braceBadLightIndicatorSet = source.braceBadLightIndicatorSet;
	braceBadLightIndicator = source.braceBadLightIndicator;

	edgeState = source.edgeState;
	theEdge = source.theEdge;
	theMultiEdge = source.theMultiEdge;

	marginNumberPadding = source.marginNumberPadding;
	ctrlCharPadding = source.ctrlCharPadding;
	lastSegItalicsOffset = source.lastSegItalicsOffset;

	wrap = source.wrap;

	localeName = source.localeName;
}

ViewStyle::~ViewStyle() {
	styles.clear();
	fonts.clear();
}

void ViewStyle::CalculateMarginWidthAndMask() noexcept {
	fixedColumnWidth = marginInside ? leftMarginWidth : 0;
	maskInLine = 0xffffffffU;
	MarkerMask maskDefinedMarkers = 0;
	for (const MarginStyle &m : ms) {
		fixedColumnWidth += m.width;
		if (m.width > 0)
			maskInLine &= ~m.mask;
		maskDefinedMarkers |= m.mask;
	}
	maskDrawInText = 0;
	for (int markBit = 0; markBit < MarkerBitCount; markBit++) {
		const MarkerMask maskBit = 1U << markBit;
		switch (markers[markBit].markType) {
		case SC_MARK_EMPTY:
			maskInLine &= ~maskBit;
			break;
		case SC_MARK_BACKGROUND:
		case SC_MARK_UNDERLINE:
			maskInLine &= ~maskBit;
			maskDrawInText |= maskDefinedMarkers & maskBit;
			break;
		default:	// Other marker types do not affect the masks
			break;
		}
	}
}

void ViewStyle::Init(size_t stylesSize_) {
	AllocStyles(stylesSize_);
	nextExtendedStyle = 256;
	fontNames.Clear();
	ResetDefaultStyle();

	// There are no image markers by default, so no need for calling CalcLargestMarkerHeight()
	largestMarkerHeight = 0;

	indicators[0] = Indicator(INDIC_SQUIGGLE, ColourAlpha(0, 0x7f, 0));
	indicators[1] = Indicator(INDIC_TT, ColourAlpha(0, 0, 0xff));
	indicators[2] = Indicator(INDIC_PLAIN, ColourAlpha(0xff, 0, 0));

	technology = SC_TECHNOLOGY_DEFAULT;
	indicatorsDynamic = false;
	indicatorsSetFore = false;
	lineHeight = 1;
	lineOverlap = 0;
	maxAscent = 1;
	maxDescent = 1;
	aveCharWidth = 8;
	spaceWidth = 8;
	tabWidth = spaceWidth * 8;

	selection.colours.fore.reset();
	selection.colours.back = ColourAlpha(0xc0, 0xc0, 0xc0);
	selection.additionalForeground = ColourAlpha(0xff, 0, 0);
	selection.additionalBackground = ColourAlpha(0xd7, 0xd7, 0xd7);
	selection.background2 = ColourAlpha(0xb0, 0xb0, 0xb0);
	selection.alpha = SC_ALPHA_NOALPHA;
	selection.additionalAlpha = SC_ALPHA_NOALPHA;
	selection.eolFilled = false;
	selection.eolSelectedWidth = 100;

	foldmarginColour.reset();
	foldmarginHighlightColour.reset();

	whitespaceColours.fore.reset();
	whitespaceColours.back.reset();
	whitespaceForeAlpha = SC_ALPHA_NOALPHA;
	controlCharSymbol = 0;	/* Draw the control characters */
	controlCharWidth = 0;
	selbar = Platform::Chrome();
	selbarlight = Platform::ChromeHighlight();
	styles[STYLE_LINENUMBER].fore = ColourAlpha(0, 0, 0);
	styles[STYLE_LINENUMBER].back = Platform::Chrome();

	caret.colour = ColourAlpha(0, 0, 0);
	caret.additionalColour = ColourAlpha(0x7f, 0x7f, 0x7f);
	caret.style = CARETSTYLE_LINE;
	caret.width = 1;

	caretLine.background = ColourAlpha(0xff, 0xff, 0);
	caretLine.show = false;
	caretLine.alwaysShow = false;
	caretLine.alpha = SC_ALPHA_NOALPHA;
	caretLine.frame = 0;

	someStylesProtected = false;
	someStylesForceCase = false;

	hotspotColours.fore.reset();
	hotspotColours.back.reset();
	hotspotUnderline = true;

	leftMarginWidth = 1;
	rightMarginWidth = 1;
	ms.resize(SC_MAX_MARGIN + 1);
	ms[0] = MarginStyle(SC_MARGIN_NUMBER);
	ms[1] = MarginStyle(SC_MARGIN_SYMBOL, 16, ~SC_MASK_FOLDERS);
	ms[2] = MarginStyle(SC_MARGIN_SYMBOL);
	marginInside = true;
	CalculateMarginWidthAndMask();
	textStart = marginInside ? fixedColumnWidth : leftMarginWidth;
	zoomLevel = 100;
	viewWhitespace = WhiteSpace::invisible;
	tabDrawMode = TabDrawMode::longArrow;
	whitespaceSize = 1;
	viewIndentationGuides = IndentView::none;
	viewEOL = false;
	extraFontFlag = 0;
	extraAscent = 0;
	extraDescent = 0;
	marginStyleOffset = 0;
	annotationVisible = ANNOTATION_HIDDEN;
	annotationStyleOffset = 0;
	eolAnnotationVisible = EOLANNOTATION_HIDDEN;
	eolAnnotationStyleOffset = 0;
	braceHighlightIndicatorSet = false;
	braceHighlightIndicator = 0;
	braceBadLightIndicatorSet = false;
	braceBadLightIndicator = 0;

	edgeState = EDGE_NONE;
	theEdge = EdgeProperties(0, ColourAlpha(0xc0, 0xc0, 0xc0));

	marginNumberPadding = 3;
	ctrlCharPadding = 3; // +3 For a blank on front and rounded edge each side
	lastSegItalicsOffset = 2;

	wrap.state = WrapMode::none;
	wrap.visualFlags = 0;
	wrap.visualFlagsLocation = 0;
	wrap.visualStartIndent = 0;
	wrap.indentMode = SC_WRAPINDENT_FIXED;

	localeName = localeNameDefault;
}

void ViewStyle::Refresh(Surface &surface, int tabInChars) {
	if (!fontsValid) {
		fontsValid = true;
		fonts.clear();

		// Apply the extra font flag which controls text drawing quality to each style.
		for (auto &style : styles) {
			style.extraFontFlag = extraFontFlag;
		}

		// Create a FontRealised object for each unique font in the styles.
		CreateAndAddFont(styles[STYLE_DEFAULT]);
		for (const auto &style : styles) {
			CreateAndAddFont(style);
		}

		// Ask platform to allocate each unique font.
		for (const auto &font : fonts) {
			font.second->Realise(surface, zoomLevel, technology, font.first, localeName.c_str());
		}

		// Set the platform font handle and measurements for each style.
		for (auto &style : styles) {
			const FontRealised *fr = Find(style);
			style.Copy(fr->font, *fr);
		}

		aveCharWidth = styles[STYLE_DEFAULT].aveCharWidth;
		spaceWidth = styles[STYLE_DEFAULT].spaceWidth;
	}

	selbar = Platform::Chrome();
	selbarlight = Platform::ChromeHighlight();

	indicatorsDynamic = std::any_of(indicators.cbegin(), indicators.cend(),
		[](const Indicator &indicator) noexcept { return indicator.IsDynamic(); });

	indicatorsSetFore = std::any_of(indicators.cbegin(), indicators.cend(),
		[](const Indicator &indicator) noexcept { return indicator.OverridesTextFore(); });

	maxAscent = 1;
	maxDescent = 1;
	FindMaxAscentDescent();
	maxAscent += extraAscent;
	maxDescent += extraDescent;
	lineHeight = maxAscent + maxDescent;
	lineOverlap = std::clamp(lineHeight / 10, 2, lineHeight);

	someStylesProtected = std::any_of(styles.cbegin(), styles.cend(),
		[](const Style &style) noexcept { return style.IsProtected(); });

	someStylesForceCase = std::any_of(styles.cbegin(), styles.cend(),
		[](const Style &style) noexcept { return style.caseForce != Style::CaseForce::mixed; });

	tabWidth = aveCharWidth * tabInChars;

	controlCharWidth = 0.0;
	if (controlCharSymbol >= 32) {
		const char cc[2] = { static_cast<char>(controlCharSymbol), '\0' };
		controlCharWidth = surface.WidthText(styles[STYLE_CONTROLCHAR].font.get(), cc);
	}

	CalculateMarginWidthAndMask();
	textStart = marginInside ? fixedColumnWidth : leftMarginWidth;
}

void ViewStyle::ReleaseAllExtendedStyles() noexcept {
	nextExtendedStyle = 256;
}

int ViewStyle::AllocateExtendedStyles(int numberStyles) {
	const int startRange = nextExtendedStyle;
	nextExtendedStyle += numberStyles;
	EnsureStyle(nextExtendedStyle);
	return startRange;
}

void ViewStyle::EnsureStyle(size_t index) {
	if (index >= styles.size()) {
		AllocStyles(index + 1);
	}
}

void ViewStyle::ResetDefaultStyle() {
	fontsValid = false;
	styles[STYLE_DEFAULT].ResetDefault(fontNames.Save(Platform::DefaultFont()));
}

void ViewStyle::ClearStyles() noexcept {
	fontsValid = false;
	// Reset all styles to be like the default style
	const Style &source = styles[STYLE_DEFAULT];
	for (auto &style : styles) {
		style.ClearTo(source);
	}

	styles[STYLE_LINENUMBER].back = Platform::Chrome();
	// Set call tip fore/back to match the values previously set for call tips
	styles[STYLE_CALLTIP].back = ColourAlpha(0xff, 0xff, 0xff);
	styles[STYLE_CALLTIP].fore = ColourAlpha(0x80, 0x80, 0x80);
}

void ViewStyle::CopyStyles(size_t sourceIndex, size_t destStyles) {
	EnsureStyle(sourceIndex);
	const Style &source = styles[sourceIndex];
	do {
		const size_t index = destStyles & 0xff;
		EnsureStyle(index);
		styles[index].ClearTo(source);
		destStyles >>= 8;
	} while (destStyles);
}

void ViewStyle::SetStyleFontName(int styleIndex, const char *name) {
	fontsValid = false;
	styles[styleIndex].fontName = fontNames.Save(name);
}

void ViewStyle::SetFontLocaleName(const char *name) {
	fontsValid = false;
	localeName = name;
}

bool ViewStyle::ProtectionActive() const noexcept {
	return someStylesProtected;
}

int ViewStyle::ExternalMarginWidth() const noexcept {
	return marginInside ? 0 : fixedColumnWidth;
}

int ViewStyle::MarginFromLocation(Point pt) const noexcept {
	int margin = -1;
	int x = marginInside ? 0 : -fixedColumnWidth;
	for (size_t i = 0; i < ms.size(); i++) {
		if ((pt.x >= x) && (pt.x < x + ms[i].width))
			margin = static_cast<int>(i);
		x += ms[i].width;
	}
	return margin;
}

bool ViewStyle::ValidStyle(size_t styleIndex) const noexcept {
	return styleIndex < styles.size();
}

void ViewStyle::CalcLargestMarkerHeight() noexcept {
	largestMarkerHeight = 0;
	for (const auto &marker : markers) {
		switch (marker.markType) {
		case SC_MARK_PIXMAP:
			if (marker.pxpm && marker.pxpm->GetHeight() > largestMarkerHeight)
				largestMarkerHeight = marker.pxpm->GetHeight();
			break;
		case SC_MARK_RGBAIMAGE:
			if (marker.image && marker.image->GetHeight() > largestMarkerHeight)
				largestMarkerHeight = marker.image->GetHeight();
			break;
		default:	// Only images have their own natural heights
			break;
		}
	}
}

int ViewStyle::GetFrameWidth() const noexcept {
	return std::clamp(caretLine.frame, 1, lineHeight / 3);
}

bool ViewStyle::IsLineFrameOpaque(bool caretActive, bool lineContainsCaret) const noexcept {
	return caretLine.frame && (caretActive || caretLine.alwaysShow) && caretLine.show &&
		(caretLine.alpha == SC_ALPHA_NOALPHA) && lineContainsCaret;
}

// See if something overrides the line background colour:  Either if caret is on the line
// and background colour is set for that, or if a marker is defined that forces its background
// colour onto the line, or if a marker is defined but has no selection margin in which to
// display itself (as long as it's not an SC_MARK_EMPTY marker).  These are checked in order
// with the earlier taking precedence.  When multiple markers cause background override,
// the colour for the highest numbered one is used.
std::optional<ColourAlpha> ViewStyle::Background(MarkerMask marksOfLine, bool caretActive, bool lineContainsCaret) const noexcept {
	std::optional<ColourAlpha> background;
	if (!caretLine.frame && (caretActive || caretLine.alwaysShow) && caretLine.show &&
		(caretLine.alpha == SC_ALPHA_NOALPHA) && lineContainsCaret) {
		background = caretLine.background;
	}
	if (!background && marksOfLine) {
		MarkerMask marks = marksOfLine;
		for (int markBit = 0; (markBit < MarkerBitCount) && marks; markBit++) {
			if ((marks & 1) && (markers[markBit].markType == SC_MARK_BACKGROUND) &&
				(markers[markBit].alpha == SC_ALPHA_NOALPHA)) {
				background = markers[markBit].back;
			}
			marks >>= 1;
		}
	}
	if (!background && maskInLine) {
		MarkerMask marksMasked = marksOfLine & maskInLine;
		if (marksMasked) {
			for (int markBit = 0; (markBit < MarkerBitCount) && marksMasked; markBit++) {
				if ((marksMasked & 1) &&
					(markers[markBit].alpha == SC_ALPHA_NOALPHA)) {
					background = markers[markBit].back;
				}
				marksMasked >>= 1;
			}
		}
	}
	return background;
}

bool ViewStyle::SelectionBackgroundDrawn() const noexcept {
	return selection.colours.back &&
		((selection.alpha == SC_ALPHA_NOALPHA) || (selection.additionalAlpha == SC_ALPHA_NOALPHA));
}

bool ViewStyle::SelectionTextDrawn() const noexcept {
	return selection.colours.fore.has_value();
}

bool ViewStyle::WhitespaceBackgroundDrawn() const noexcept {
	return (viewWhitespace != WhiteSpace::invisible) && (whitespaceColours.back);
}

bool ViewStyle::WhiteSpaceVisible(bool inIndent) const noexcept {
	return (!inIndent && viewWhitespace == WhiteSpace::visibleAfterIndent) ||
		(inIndent && viewWhitespace == WhiteSpace::visibleOnlyInIndent) ||
		viewWhitespace == WhiteSpace::visibleAlways;
}

ColourAlpha ViewStyle::WrapColour() const noexcept {
	return whitespaceColours.fore.value_or(styles[STYLE_DEFAULT].fore);
}

// Insert new edge in sorted order.
void ViewStyle::AddMultiEdge(uptr_t wParam, sptr_t lParam) {
	const int column = static_cast<int>(wParam);
	theMultiEdge.insert(
		std::upper_bound(theMultiEdge.begin(), theMultiEdge.end(), column,
		[](const EdgeProperties &a, const EdgeProperties &b) noexcept {
			return a.column < b.column;
		}),
		EdgeProperties(column, lParam));
}

std::optional<ColourAlpha> ViewStyle::ElementColour(int element) const {
	const auto search = elementColours.find(element);
	if (search != elementColours.end()) {
		if (search->second.has_value()) {
			return search->second;
		}
	}
	return {};
}

bool ViewStyle::ElementAllowsTranslucent(int element) const {
	return elementAllowsTranslucent.count(element) > 0;
}

bool ViewStyle::SetWrapState(int wrapState_) noexcept {
	WrapMode wrapStateWanted;
	switch (wrapState_) {
	case SC_WRAP_WORD:
		wrapStateWanted = WrapMode::word;
		break;
	case SC_WRAP_CHAR:
		wrapStateWanted = WrapMode::character;
		break;
	case SC_WRAP_WHITESPACE:
		wrapStateWanted = WrapMode::whitespace;
		break;
	case SC_WRAP_AUTO:
		wrapStateWanted = WrapMode::automatic;
		break;
	default:
		wrapStateWanted = WrapMode::none;
		break;
	}
	const bool changed = wrap.state != wrapStateWanted;
	wrap.state = wrapStateWanted;
	return changed;
}

bool ViewStyle::SetWrapVisualFlags(int wrapVisualFlags_) noexcept {
	const bool changed = wrap.visualFlags != wrapVisualFlags_;
	wrap.visualFlags = wrapVisualFlags_;
	return changed;
}

bool ViewStyle::SetWrapVisualFlagsLocation(int wrapVisualFlagsLocation_) noexcept {
	const bool changed = wrap.visualFlagsLocation != wrapVisualFlagsLocation_;
	wrap.visualFlagsLocation = wrapVisualFlagsLocation_;
	return changed;
}

bool ViewStyle::SetWrapVisualStartIndent(int wrapVisualStartIndent_) noexcept {
	const bool changed = wrap.visualStartIndent != wrapVisualStartIndent_;
	wrap.visualStartIndent = wrapVisualStartIndent_;
	return changed;
}

bool ViewStyle::SetWrapIndentMode(int wrapIndentMode_) noexcept {
	const bool changed = wrap.indentMode != wrapIndentMode_;
	wrap.indentMode = wrapIndentMode_;
	return changed;
}

bool ViewStyle::IsBlockCaretStyle() const noexcept {
	return ((caret.style & CARETSTYLE_INS_MASK) == CARETSTYLE_BLOCK) ||
		(caret.style & CARETSTYLE_OVERSTRIKE_BLOCK) != 0;
}

bool ViewStyle::IsCaretVisible() const noexcept {
	return caret.width > 0 && caret.style != CARETSTYLE_INVISIBLE;
}

bool ViewStyle::DrawCaretInsideSelection(bool inOverstrike, bool imeCaretBlockOverride) const noexcept {
	if (caret.style & CARETSTYLE_BLOCK_AFTER) {
		return false;
	}
	return ((caret.style & CARETSTYLE_INS_MASK) == CARETSTYLE_BLOCK) ||
		(inOverstrike && (caret.style & CARETSTYLE_OVERSTRIKE_BLOCK) != 0) ||
		imeCaretBlockOverride;
}

ViewStyle::CaretShape ViewStyle::CaretShapeForMode(bool inOverstrike, bool drawDrag, bool drawOverstrikeCaret, bool imeCaretBlockOverride) const noexcept {
	if (drawDrag) {
		// Dragging text, use a line caret
		return CaretShape::line;
	}
	if (inOverstrike) {
		if (caret.style & CARETSTYLE_OVERSTRIKE_BLOCK) {
			return CaretShape::block;
		}
		if (drawOverstrikeCaret) {
			return CaretShape::bar;
		}
	}
	if (imeCaretBlockOverride) {
		return CaretShape::block;
	}
	const int style = caret.style & CARETSTYLE_INS_MASK;
	return (style <= CARETSTYLE_BLOCK) ? static_cast<CaretShape>(style) : CaretShape::line;
}

bool ViewStyle::ZoomIn() noexcept {
	if (zoomLevel < SC_MAX_ZOOM_LEVEL) {
		int level = zoomLevel;
		if (level < 200) {
			level += 10;
		} else {
			level += 25;
		}

		level = std::min(level, SC_MAX_ZOOM_LEVEL);
		if (level != zoomLevel) {
			zoomLevel = level;
			fontsValid = false;
			return true;
		}
	}
	return false;
}

bool ViewStyle::ZoomOut() noexcept {
	if (zoomLevel > SC_MIN_ZOOM_LEVEL) {
		int level = zoomLevel;
		if (level <= 200) {
			level -= 10;
		} else {
			level -= 25;
		}

		level = std::max(level, SC_MIN_ZOOM_LEVEL);
		if (level != zoomLevel) {
			zoomLevel = level;
			fontsValid = false;
			return true;
		}
	}
	return false;
}

void ViewStyle::AllocStyles(size_t sizeNew) {
	fontsValid = false;
	const size_t i = styles.size();
	styles.resize(sizeNew);
	sizeNew = styles.size();
	if (sizeNew > STYLE_DEFAULT) {
		const Style &source = styles[STYLE_DEFAULT];
		for (auto it = styles.begin() + i; it != styles.end(); ++it) {
			it->ClearTo(source);
		}
	}
}

void ViewStyle::CreateAndAddFont(const FontSpecification &fs) {
	if (fs.fontName) {
		const auto it = fonts.find(fs);
		if (it == fonts.end()) {
			fonts.emplace(fs, std::make_unique<FontRealised>());
		}
	}
}

FontRealised *ViewStyle::Find(const FontSpecification &fs) const {
	if (!fs.fontName)	// Invalid specification so return arbitrary object
		return fonts.begin()->second.get();
	const auto it = fonts.find(fs);
	if (it != fonts.end()) {
		// Should always reach here since map was just set for all styles
		return it->second.get();
	}
	return nullptr;
}

void ViewStyle::FindMaxAscentDescent() {
	auto ascent = maxAscent;
	auto descent = maxDescent;
	for (const auto &font : fonts) {
		ascent = std::max(ascent, font.second->ascent);
		descent = std::max(descent, font.second->descent);
	}
	maxAscent = ascent;
	maxDescent = descent;
}
