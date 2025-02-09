// Scintilla source code edit control
/** @file Editor.cxx
 ** Main code for the edit control.
 **/
// Copyright 1998-2011 by Neil Hodgson <neilh@scintilla.org>
// The License.txt file describes the conditions under which this software may be distributed.

#include <cstddef>
#include <cstdlib>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cmath>

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <set>
#include <forward_list>
#include <optional>
#include <algorithm>
#include <iterator>
#include <memory>
#include <chrono>

#include "Debugging.h"
#include "Geometry.h"
#include "Platform.h"

#include "ILoader.h"
#include "ILexer.h"
#include "Scintilla.h"

#include "CharacterSet.h"
//#include "CharacterCategory.h"
#include "Position.h"
#include "UniqueString.h"
#include "SplitVector.h"
#include "Partitioning.h"
#include "RunStyles.h"
#include "ContractionState.h"
#include "CellBuffer.h"
#include "PerLine.h"
#include "KeyMap.h"
#include "Indicator.h"
#include "LineMarker.h"
#include "Style.h"
#include "ViewStyle.h"
#include "CharClassify.h"
#include "Decoration.h"
#include "CaseFolder.h"
#include "Document.h"
#include "UniConversion.h"
#include "Selection.h"
#include "PositionCache.h"
#include "EditModel.h"
#include "MarginView.h"
#include "EditView.h"
#include "Editor.h"
#include "ElapsedPeriod.h"

using namespace Scintilla;
using namespace Lexilla;

namespace {

/*
	return whether this modification represents an operation that
	may reasonably be deferred (not done now OR [possibly] at all)
*/
constexpr bool CanDeferToLastStep(const DocModification &mh) noexcept {
	if (mh.modificationType & (SC_MOD_BEFOREINSERT | SC_MOD_BEFOREDELETE))
		return true;	// CAN skip
	if (!(mh.modificationType & (SC_PERFORMED_UNDO | SC_PERFORMED_REDO)))
		return false;	// MUST do
	if (mh.modificationType & SC_MULTISTEPUNDOREDO)
		return true;	// CAN skip
	return false;		// PRESUMABLY must do
}

constexpr bool CanEliminate(const DocModification &mh) noexcept {
	return
		(mh.modificationType & (SC_MOD_BEFOREINSERT | SC_MOD_BEFOREDELETE)) != 0;
}

/*
	return whether this modification represents the FINAL step
	in a [possibly lengthy] multi-step Undo/Redo sequence
*/
constexpr bool IsLastStep(const DocModification &mh) noexcept {
	return
		(mh.modificationType & (SC_PERFORMED_UNDO | SC_PERFORMED_REDO)) != 0
		&& (mh.modificationType & SC_MULTISTEPUNDOREDO) != 0
		&& (mh.modificationType & SC_LASTSTEPINUNDOREDO) != 0
		&& (mh.modificationType & SC_MULTILINEUNDOREDO) != 0;
}

}

Timer::Timer() noexcept :
		ticking(false), ticksToWait(0), tickerID{} {}

Idler::Idler() noexcept :
		state(false), idlerID(nullptr) {}

static constexpr bool IsAllSpacesOrTabs(std::string_view sv) noexcept {
	for (const char ch : sv) {
		// This is safe because IsSpaceOrTab() will return false for null terminators
		if (!IsSpaceOrTab(ch))
			return false;
	}
	return true;
}

Editor::Editor() {
	ctrlID = 0;

	stylesValid = false;
	technology = SC_TECHNOLOGY_DEFAULT;
	scaleRGBAImage = 100.0f;

	cursorMode = SC_CURSORNORMAL;

	errorStatus = 0;
	mouseDownCaptures = true;
	mouseWheelCaptures = true;

	lastClickTime = 0;
	doubleClickCloseThreshold = Point(3, 3);
	dwellDelay = SC_TIME_FOREVER;
	ticksToDwell = SC_TIME_FOREVER;
	dwelling = false;
	ptMouseLast.x = 0;
	ptMouseLast.y = 0;
	inDragDrop = DragDrop::none;
	dropWentOutside = false;
	posDrop = SelectionPosition(Sci::invalidPosition);
	hotSpotClickPos = INVALID_POSITION;
	selectionUnit = TextUnit::character;

	lastXChosen = 0;
	lineAnchorPos = 0;
	originalAnchorPos = 0;
	wordSelectAnchorStartPos = 0;
	wordSelectAnchorEndPos = 0;
	wordSelectInitialCaretPos = -1;

	caretPolicies.x = { CARET_SLOP | CARET_EVEN, 50 };
	caretPolicies.y = { CARET_EVEN, 0 };

	visiblePolicy = { 0, 0 };

	searchAnchor = 0;

	xCaretMargin = 50;
	horizontalScrollBarVisible = true;
	scrollWidth = 2000;
	verticalScrollBarVisible = true;
	endAtLastLine = 1;
	caretSticky = SC_CARETSTICKY_OFF;
	marginOptions = SC_MARGINOPTION_NONE;
	mouseSelectionRectangularSwitch = false;
	multipleSelection = false;
	additionalSelectionTyping = false;
	multiPasteMode = SC_MULTIPASTE_ONCE;
	virtualSpaceOptions = SCVS_NONE;

	targetRange = SelectionSegment();
	searchFlags = 0;

	topLine = 0;
	posTopLine = 0;

	lengthForEncode = -1;

	needUpdateUI = 0;
	ContainerNeedsUpdate(SC_UPDATE_CONTENT);

	paintState = PaintState::notPainting;
	paintAbandonedByStyling = false;
	paintingAllText = false;
	willRedrawAll = false;
	idleStyling = SC_IDLESTYLING_NONE;
	needIdleStyling = false;

	modEventMask = SC_MODEVENTMASKALL;
	commandEvents = true;

	pdoc->AddWatcher(this, nullptr);

	recordingMacro = false;
	foldAutomatic = 0;

	convertPastes = true;

	SetRepresentations();
}

Editor::~Editor() {
	pdoc->RemoveWatcher(this, nullptr);
	DropGraphics();
}

void Editor::Finalise() noexcept {
	SetIdle(false);
	CancelModes();
}

void Editor::SetRepresentations() {
	reprs.Clear();

	// C0 control set
	static const char * const reps[] = {
		"NUL", "SOH", "STX", "ETX", "EOT", "ENQ", "ACK", "BEL",
		"BS", "HT", "LF", "VT", "FF", "CR", "SO", "SI",
		"DLE", "DC1", "DC2", "DC3", "DC4", "NAK", "SYN", "ETB",
		"CAN", "EM", "SUB", "ESC", "FS", "GS", "RS", "US"
	};
	for (size_t j = 0; j < std::size(reps); j++) {
		const char c[2] = { static_cast<char>(j), 0 };
		reprs.SetRepresentation(c, reps[j]);
	}
	reprs.SetRepresentation("\x7f", "DEL");

	// C1 control set
	// As well as Unicode mode, ISO-8859-1 should use these
	if (IsUnicodeMode()) {
		static const char * const repsC1[] = {
			"PAD", "HOP", "BPH", "NBH", "IND", "NEL", "SSA", "ESA",
			"HTS", "HTJ", "VTS", "PLD", "PLU", "RI", "SS2", "SS3",
			"DCS", "PU1", "PU2", "STS", "CCH", "MW", "SPA", "EPA",
			"SOS", "SGCI", "SCI", "CSI", "ST", "OSC", "PM", "APC"
		};
		for (size_t j = 0; j < std::size(repsC1); j++) {
			const char c1[3] = { '\xc2',  static_cast<char>(0x80 + j), 0 };
			reprs.SetRepresentation(c1, repsC1[j]);
		}
		reprs.SetRepresentation("\xe2\x80\xa8", "LS");
		reprs.SetRepresentation("\xe2\x80\xa9", "PS");

		// UTF-8 invalid bytes
		for (int k = 0x80; k < 0x100; k++) {
			const char hiByte[2] = { static_cast<char>(k), 0 };
			char hexits[5];	// Really only needs 4 but that causes warning from gcc 7.1
			sprintf(hexits, "x%2X", k);
			reprs.SetRepresentation(hiByte, hexits);
		}
	} else if (pdoc->dbcsCodePage) {
		// DBCS invalid single lead bytes
		for (int k = 0x80; k < 0x100; k++) {
			const char ch = static_cast<char>(k);
			if (pdoc->IsDBCSLeadByteNoExcept(ch) || pdoc->IsDBCSLeadByteInvalid(ch)) {
				const char hiByte[2] = { ch, 0 };
				char hexits[5];	// Really only needs 4 but that causes warning from gcc 7.1
				sprintf(hexits, "x%2X", k);
				reprs.SetRepresentation(hiByte, hexits);
			}
		}
	}
}

void Editor::DropGraphics() noexcept {
	marginView.DropGraphics();
	view.DropGraphics();
}

void Editor::InvalidateStyleData() {
	stylesValid = false;
	vs.technology = technology;
	DropGraphics();
	view.llc.Invalidate(LineLayout::ValidLevel::invalid);
	view.posCache.Clear();
}

void Editor::InvalidateStyleRedraw() {
	NeedWrapping();
	InvalidateStyleData();
	Redraw();
}

void Editor::RefreshStyleData() {
	if (!stylesValid) {
		stylesValid = true;
		AutoSurface surface(this);
		if (surface) {
			vs.Refresh(*surface, pdoc->tabInChars);
		}
		SetScrollBars();
		SetRectangularRange();
	}
}

Point Editor::GetVisibleOriginInMain() const noexcept {
	return Point(0, 0);
}

PointDocument Editor::DocumentPointFromView(Point ptView) const noexcept {
	PointDocument ptDocument(ptView);
	if (wMargin.GetID()) {
		const Point ptOrigin = GetVisibleOriginInMain();
		ptDocument.x += ptOrigin.x;
		ptDocument.y += ptOrigin.y;
	} else {
		ptDocument.x += xOffset;
		ptDocument.y += topLine * vs.lineHeight;
	}
	return ptDocument;
}

Sci::Line Editor::TopLineOfMain() const noexcept {
	if (wMargin.GetID())
		return 0;
	else
		return topLine;
}

PRectangle Editor::GetClientRectangle() const noexcept {
	return wMain.GetClientPosition();
}

PRectangle Editor::GetClientDrawingRectangle() const noexcept {
	return GetClientRectangle();
}

PRectangle Editor::GetTextRectangle() const noexcept {
	PRectangle rc = GetClientRectangle();
	rc.left += vs.textStart;
	rc.right -= vs.rightMarginWidth;
	return rc;
}

Sci::Line Editor::LinesOnScreen() const noexcept {
	const PRectangle rcClient = GetClientRectangle();
	const int htClient = static_cast<int>(rcClient.bottom - rcClient.top);
	//Platform::DebugPrintf("lines on screen = %d\n", htClient / vs.lineHeight + 1);
	return htClient / vs.lineHeight;
}

Sci::Line Editor::LinesToScroll() const noexcept {
	const Sci::Line retVal = LinesOnScreen() - 1;
	if (retVal < 1)
		return 1;
	else
		return retVal;
}

Sci::Line Editor::MaxScrollPos() const noexcept {
	Sci::Line retVal = pcs->LinesDisplayed();
	const Sci::Line linesOnScreen = LinesOnScreen();
	//Platform::DebugPrintf("Lines %d screen = %d maxScroll = %d\n",
	//pdoc->LinesTotal(), linesOnScreen, pdoc->LinesTotal() - linesOnScreen + 1);
	switch (endAtLastLine) {
	case 0:
		retVal--;
		break;
	case 1:
		retVal -= linesOnScreen;
		break;
	case 2:
		retVal -= linesOnScreen/2;
		break;
	case 3:
		retVal -= linesOnScreen - linesOnScreen/3;
		break;
	case 4:
		retVal -= linesOnScreen - linesOnScreen/4;
		break;
	}
	return std::max<Sci::Line>(retVal, 0);
}

SelectionPosition Editor::ClampPositionIntoDocument(SelectionPosition sp) const noexcept {
	if (sp.Position() < 0) {
		return SelectionPosition(0);
	} else if (sp.Position() > pdoc->Length()) {
		return SelectionPosition(pdoc->Length());
	} else {
		// If not at end of line then set offset to 0
		if (!pdoc->IsLineEndPosition(sp.Position()))
			sp.SetVirtualSpace(0);
		return sp;
	}
}

Point Editor::LocationFromPosition(SelectionPosition pos, PointEnd pe) {
	const PRectangle rcClient = GetTextRectangle();
	RefreshStyleData();
	AutoSurface surface(this);
	return view.LocationFromPosition(surface, *this, pos, topLine, vs, pe, rcClient);
}

Point Editor::LocationFromPosition(Sci::Position pos, PointEnd pe) {
	return LocationFromPosition(SelectionPosition(pos), pe);
}

int Editor::XFromPosition(SelectionPosition sp) {
	const Point pt = LocationFromPosition(sp);
	return static_cast<int>(pt.x) - vs.textStart + xOffset;
}

SelectionPosition Editor::SPositionFromLocation(Point pt, bool canReturnInvalid, bool charPosition, bool virtualSpace) {
	RefreshStyleData();
	AutoSurface surface(this);

	PRectangle rcClient = GetTextRectangle();
	// May be in scroll view coordinates so translate back to main view
	const Point ptOrigin = GetVisibleOriginInMain();
	rcClient.Move(-ptOrigin.x, -ptOrigin.y);

	if (canReturnInvalid) {
		if (!rcClient.Contains(pt))
			return SelectionPosition(INVALID_POSITION);
		if (pt.x < vs.textStart)
			return SelectionPosition(INVALID_POSITION);
		if (pt.y < 0)
			return SelectionPosition(INVALID_POSITION);
	}
	const PointDocument ptdoc = DocumentPointFromView(pt);
	return view.SPositionFromLocation(surface, *this, ptdoc, canReturnInvalid,
		charPosition, virtualSpace, vs, rcClient);
}

Sci::Position Editor::PositionFromLocation(Point pt, bool canReturnInvalid, bool charPosition) {
	return SPositionFromLocation(pt, canReturnInvalid, charPosition, false).Position();
}

/**
* Find the document position corresponding to an x coordinate on a particular document line.
* Ensure is between whole characters when document is in multi-byte or UTF-8 mode.
* This method is used for rectangular selections and does not work on wrapped lines.
*/
SelectionPosition Editor::SPositionFromLineX(Sci::Line lineDoc, int x) {
	RefreshStyleData();
	if (lineDoc >= pdoc->LinesTotal())
		return SelectionPosition(pdoc->Length());
	//Platform::DebugPrintf("Position of (%d) line = %d top=%d\n", x, lineDoc, topLine);
	AutoSurface surface(this);
	return view.SPositionFromLineX(surface, *this, lineDoc, x, vs);
}

Sci::Position Editor::PositionFromLineX(Sci::Line lineDoc, int x) {
	return SPositionFromLineX(lineDoc, x).Position();
}

Sci::Line Editor::LineFromLocation(Point pt) const noexcept {
	return pcs->DocFromDisplay(static_cast<int>(pt.y) / vs.lineHeight + topLine);
}

void Editor::SetTopLine(Sci::Line topLineNew) noexcept {
	if ((topLine != topLineNew) && (topLineNew >= 0)) {
		topLine = topLineNew;
		ContainerNeedsUpdate(SC_UPDATE_V_SCROLL);
	}
	posTopLine = pdoc->LineStart(pcs->DocFromDisplay(topLine));
}

/**
 * If painting then abandon the painting because a wider redraw is needed.
 * @return true if calling code should stop drawing.
 */
bool Editor::AbandonPaint() noexcept {
	if ((paintState == PaintState::painting) && !paintingAllText) {
		paintState = PaintState::abandoned;
	}
	return paintState == PaintState::abandoned;
}

void Editor::RedrawRect(PRectangle rc) noexcept {
	//Platform::DebugPrintf("Redraw %.0f,%.0f - %.0f,%.0f\n", rc.left, rc.top, rc.right, rc.bottom);

	// Clip the redraw rectangle into the client area
	const PRectangle rcClient = GetClientRectangle();
	if (rc.top < rcClient.top)
		rc.top = rcClient.top;
	if (rc.bottom > rcClient.bottom)
		rc.bottom = rcClient.bottom;
	if (rc.left < rcClient.left)
		rc.left = rcClient.left;
	if (rc.right > rcClient.right)
		rc.right = rcClient.right;

	if ((rc.bottom > rc.top) && (rc.right > rc.left)) {
		wMain.InvalidateRectangle(rc);
	}
}

void Editor::DiscardOverdraw() noexcept {
	// Overridden on platforms that may draw outside visible area.
}

void Editor::Redraw() noexcept {
	//Platform::DebugPrintf("Redraw all\n");
	const PRectangle rcClient = GetClientRectangle();
	wMain.InvalidateRectangle(rcClient);
	if (wMargin.GetID())
		wMargin.InvalidateAll();
	//wMain.InvalidateAll();
}

void Editor::RedrawSelMargin(Sci::Line line, bool allAfter) noexcept {
	const bool markersInText = vs.maskInLine || vs.maskDrawInText;
	if (!wMargin.GetID() || markersInText) {	// May affect text area so may need to abandon and retry
		if (AbandonPaint()) {
			return;
		}
	}
	if (wMargin.GetID() && markersInText) {
		Redraw();
		return;
	}
	PRectangle rcMarkers = GetClientRectangle();
	if (!markersInText) {
		// Normal case: just draw the margin
		rcMarkers.right = rcMarkers.left + vs.fixedColumnWidth;
	}
	if (line != -1) {
		PRectangle rcLine = RectangleFromRange(Range(pdoc->LineStart(line)), 0);

		// Inflate line rectangle if there are image markers with height larger than line height
		if (vs.largestMarkerHeight > vs.lineHeight) {
			const int delta = (vs.largestMarkerHeight - vs.lineHeight + 1) / 2;
			rcLine.top = std::max(rcLine.top - delta, rcMarkers.top);
			rcLine.bottom = std::min(rcLine.bottom + delta, rcMarkers.bottom);
		}

		rcMarkers.top = rcLine.top;
		if (!allAfter)
			rcMarkers.bottom = rcLine.bottom;
		if (rcMarkers.Empty())
			return;
	}
	if (wMargin.GetID()) {
		const Point ptOrigin = GetVisibleOriginInMain();
		rcMarkers.Move(-ptOrigin.x, -ptOrigin.y);
		wMargin.InvalidateRectangle(rcMarkers);
	} else {
		wMain.InvalidateRectangle(rcMarkers);
	}
}

PRectangle Editor::RectangleFromRange(Range r, int overlap) const noexcept {
	const Sci::Line minLine = pcs->DisplayFromDoc(
		pdoc->SciLineFromPosition(r.First()));
	const Sci::Line maxLine = pcs->DisplayLastFromDoc(
		pdoc->SciLineFromPosition(r.Last()));
	const PRectangle rcClientDrawing = GetClientDrawingRectangle();
	PRectangle rc;
	const int leftTextOverlap = ((xOffset == 0) && (vs.leftMarginWidth > 0)) ? 1 : 0;
	rc.left = static_cast<XYPOSITION>(vs.textStart - leftTextOverlap);
	rc.top = static_cast<XYPOSITION>((minLine - TopLineOfMain()) * vs.lineHeight - overlap);
	rc.top = std::max(rc.top, rcClientDrawing.top);
	// Extend to right of prepared area if any to prevent artifacts from caret line highlight
	rc.right = rcClientDrawing.right;
	rc.bottom = static_cast<XYPOSITION>((maxLine - TopLineOfMain() + 1) * vs.lineHeight + overlap);

	return rc;
}

void Editor::InvalidateRange(Sci::Position start, Sci::Position end) noexcept {
	RedrawRect(RectangleFromRange(Range(start, end), view.LinesOverlap() ? vs.lineOverlap : 0));
}

Sci::Position Editor::CurrentPosition() const noexcept {
	return sel.MainCaret();
}

bool Editor::SelectionEmpty() const noexcept {
	return sel.Empty();
}

SelectionPosition Editor::SelectionStart() noexcept {
	return sel.RangeMain().Start();
}

SelectionPosition Editor::SelectionEnd() noexcept {
	return sel.RangeMain().End();
}

void Editor::SetRectangularRange() {
	if (sel.IsRectangular()) {
		const int xAnchor = XFromPosition(sel.Rectangular().anchor);
		int xCaret = XFromPosition(sel.Rectangular().caret);
		if (sel.selType == Selection::SelTypes::thin) {
			xCaret = xAnchor;
		}
		const Sci::Line lineAnchorRect =
			pdoc->SciLineFromPosition(sel.Rectangular().anchor.Position());
		const Sci::Line lineCaret =
			pdoc->SciLineFromPosition(sel.Rectangular().caret.Position());
		const int increment = (lineCaret > lineAnchorRect) ? 1 : -1;
		AutoSurface surface(this);
		for (Sci::Line line = lineAnchorRect; line != lineCaret + increment; line += increment) {
			SelectionRange range(
				view.SPositionFromLineX(surface, *this, line, xCaret, vs),
				view.SPositionFromLineX(surface, *this, line, xAnchor, vs));
			if ((virtualSpaceOptions & SCVS_RECTANGULARSELECTION) == 0)
				range.ClearVirtualSpace();
			if (line == lineAnchorRect)
				sel.SetSelection(range);
			else
				sel.AddSelectionWithoutTrim(range);
		}
	}
}

void Editor::ThinRectangularRange() {
	if (sel.IsRectangular()) {
		sel.selType = Selection::SelTypes::thin;
		if (sel.Rectangular().caret < sel.Rectangular().anchor) {
			sel.Rectangular() = SelectionRange(sel.Range(sel.Count() - 1).caret, sel.Range(0).anchor);
		} else {
			sel.Rectangular() = SelectionRange(sel.Range(sel.Count() - 1).anchor, sel.Range(0).caret);
		}
		SetRectangularRange();
	}
}

void Editor::InvalidateSelection(SelectionRange newMain, bool invalidateWholeSelection) noexcept {
	if (sel.Count() > 1 || !(sel.RangeMain().anchor == newMain.anchor) || sel.IsRectangular()) {
		invalidateWholeSelection = true;
	}
	Sci::Position firstAffected = std::min(sel.RangeMain().Start().Position(), newMain.Start().Position());
	// +1 for lastAffected ensures caret repainted
	Sci::Position lastAffected = std::max(newMain.caret.Position() + 1, newMain.anchor.Position());
	lastAffected = std::max(lastAffected, sel.RangeMain().End().Position());
	if (invalidateWholeSelection) {
		for (size_t r = 0; r < sel.Count(); r++) {
			firstAffected = std::min(firstAffected, sel.Range(r).caret.Position());
			firstAffected = std::min(firstAffected, sel.Range(r).anchor.Position());
			lastAffected = std::max(lastAffected, sel.Range(r).caret.Position() + 1);
			lastAffected = std::max(lastAffected, sel.Range(r).anchor.Position());
		}
	}
	ContainerNeedsUpdate(SC_UPDATE_SELECTION);
	InvalidateRange(firstAffected, lastAffected);
}

void Editor::InvalidateWholeSelection() noexcept {
	InvalidateSelection(sel.RangeMain(), true);
}

/* For Line selection - the anchor and caret are always
   at the beginning and end of the region lines. */
SelectionRange Editor::LineSelectionRange(SelectionPosition currentPos_, SelectionPosition anchor_, bool withEOL) const noexcept {
	if (currentPos_ >= anchor_) {
		anchor_ = SelectionPosition(
			pdoc->LineStart(pdoc->LineFromPosition(anchor_.Position())));
		const Sci::Line endLine = pdoc->LineFromPosition(currentPos_.Position());
		currentPos_ = SelectionPosition(
			withEOL ? pdoc->LineStart(endLine + 1) : pdoc->LineEnd(endLine));
	} else {
		currentPos_ = SelectionPosition(
			pdoc->LineStart(pdoc->LineFromPosition(currentPos_.Position())));
		const Sci::Line endLine = pdoc->LineFromPosition(anchor_.Position());
		anchor_ = SelectionPosition(
			withEOL ? pdoc->LineStart(endLine + 1) : pdoc->LineEnd(endLine));
	}
	return SelectionRange(currentPos_, anchor_);
}

void Editor::SetSelection(SelectionPosition currentPos_, SelectionPosition anchor_) {
	currentPos_ = ClampPositionIntoDocument(currentPos_);
	anchor_ = ClampPositionIntoDocument(anchor_);
	const Sci::Line currentLine = pdoc->SciLineFromPosition(currentPos_.Position());
	SelectionRange rangeNew(currentPos_, anchor_);
	if (sel.selType == Selection::SelTypes::lines) {
		rangeNew = LineSelectionRange(currentPos_, anchor_);
	}
	if (sel.Count() > 1 || !(sel.RangeMain() == rangeNew)) {
		InvalidateSelection(rangeNew);
	}
	sel.RangeMain() = rangeNew;
	SetRectangularRange();
	ClaimSelection();
	SetHoverIndicatorPosition(sel.MainCaret());

	if (marginView.highlightDelimiter.NeedsDrawing(currentLine)) {
		RedrawSelMargin();
	}
	QueueIdleWork(WorkItems::updateUI);
}

void Editor::SetSelection(Sci::Position currentPos_, Sci::Position anchor_) {
	SetSelection(SelectionPosition(currentPos_), SelectionPosition(anchor_));
}

// Just move the caret on the main selection
void Editor::SetSelection(SelectionPosition currentPos_) {
	currentPos_ = ClampPositionIntoDocument(currentPos_);
	const Sci::Line currentLine = pdoc->SciLineFromPosition(currentPos_.Position());
	if (sel.Count() > 1 || !(sel.RangeMain().caret == currentPos_)) {
		InvalidateSelection(SelectionRange(currentPos_));
	}
	if (sel.IsRectangular()) {
		sel.Rectangular() =
			SelectionRange(SelectionPosition(currentPos_), sel.Rectangular().anchor);
		SetRectangularRange();
	} else if (sel.selType == Selection::SelTypes::lines) {
		sel.RangeMain() = LineSelectionRange(currentPos_, sel.RangeMain().anchor);
	} else {
		sel.RangeMain() =
			SelectionRange(SelectionPosition(currentPos_), sel.RangeMain().anchor);
	}
	ClaimSelection();
	SetHoverIndicatorPosition(sel.MainCaret());

	if (marginView.highlightDelimiter.NeedsDrawing(currentLine)) {
		RedrawSelMargin();
	}
	QueueIdleWork(WorkItems::updateUI);
}

void Editor::SetEmptySelection(SelectionPosition currentPos_) {
	const Sci::Line currentLine = pdoc->SciLineFromPosition(currentPos_.Position());
	SelectionRange rangeNew(ClampPositionIntoDocument(currentPos_));
	if (sel.Count() > 1 || !(sel.RangeMain() == rangeNew)) {
		InvalidateSelection(rangeNew);
	}
	sel.Clear();
	sel.RangeMain() = rangeNew;
	SetRectangularRange();
	ClaimSelection();
	SetHoverIndicatorPosition(sel.MainCaret());

	if (marginView.highlightDelimiter.NeedsDrawing(currentLine)) {
		RedrawSelMargin();
	}
	QueueIdleWork(WorkItems::updateUI);
}

void Editor::SetEmptySelection(Sci::Position currentPos_) {
	SetEmptySelection(SelectionPosition(currentPos_));
}

void Editor::MultipleSelectAdd(AddNumber addNumber) {
	if (SelectionEmpty() || !multipleSelection) {
		// Select word at caret
		const Sci::Position startWord = pdoc->ExtendWordSelect(sel.MainCaret(), -1, true);
		const Sci::Position endWord = pdoc->ExtendWordSelect(startWord, 1, true);
		TrimAndSetSelection(endWord, startWord);

	} else {

		if (!pdoc->HasCaseFolder())
			pdoc->SetCaseFolder(CaseFolderForEncoding());

		const Range rangeMainSelection(sel.RangeMain().Start().Position(), sel.RangeMain().End().Position());
		const std::string selectedText = RangeText(rangeMainSelection.start, rangeMainSelection.end);

		const Range rangeTarget(targetRange.start.Position(), targetRange.end.Position());
		std::vector<Range> searchRanges;
		// Search should be over the target range excluding the current selection so
		// may need to search 2 ranges, after the selection then before the selection.
		if (rangeTarget.Overlaps(rangeMainSelection)) {
			// Common case is that the selection is completely within the target but
			// may also have overlap at start or end.
			if (rangeMainSelection.end < rangeTarget.end)
				searchRanges.emplace_back(rangeMainSelection.end, rangeTarget.end);
			if (rangeTarget.start < rangeMainSelection.start)
				searchRanges.emplace_back(rangeTarget.start, rangeMainSelection.start);
		} else {
			// No overlap
			searchRanges.push_back(rangeTarget);
		}

		for (const auto & searchRange : searchRanges) {
			Sci::Position searchStart = searchRange.start;
			const Sci::Position searchEnd = searchRange.end;
			for (;;) {
				Sci::Position lengthFound = selectedText.length();
				const Sci::Position pos = pdoc->FindText(searchStart, searchEnd,
					selectedText.c_str(), searchFlags, &lengthFound);
				if (pos >= 0) {
					sel.AddSelection(SelectionRange(pos + lengthFound, pos));
					ContainerNeedsUpdate(SC_UPDATE_SELECTION);
					ScrollRange(sel.RangeMain());
					Redraw();
					if (addNumber == AddNumber::one)
						return;
					searchStart = pos + lengthFound;
				} else {
					break;
				}
			}
		}
	}
}

bool Editor::RangeContainsProtected(Sci::Position start, Sci::Position end) const noexcept {
	if (vs.ProtectionActive()) {
		if (start > end) {
			const Sci::Position t = start;
			start = end;
			end = t;
		}
		for (Sci::Position pos = start; pos < end; pos++) {
			if (vs.styles[pdoc->StyleIndexAt(pos)].IsProtected())
				return true;
		}
	}
	return false;
}

bool Editor::SelectionContainsProtected() const noexcept {
	for (size_t r = 0; r < sel.Count(); r++) {
		if (RangeContainsProtected(sel.Range(r).Start().Position(),
			sel.Range(r).End().Position())) {
			return true;
		}
	}
	return false;
}

/**
 * Asks document to find a good position and then moves out of any invisible positions.
 */
Sci::Position Editor::MovePositionOutsideChar(Sci::Position pos, Sci::Position moveDir, bool checkLineEnd) const noexcept {
	return MovePositionOutsideChar(SelectionPosition(pos), moveDir, checkLineEnd).Position();
}

SelectionPosition Editor::MovePositionOutsideChar(SelectionPosition pos, Sci::Position moveDir, bool checkLineEnd) const noexcept {
	const Sci::Position posMoved = pdoc->MovePositionOutsideChar(pos.Position(), moveDir, checkLineEnd);
	if (posMoved != pos.Position())
		pos.SetPosition(posMoved);
	if (vs.ProtectionActive()) {
		if (moveDir > 0) {
			if ((pos.Position() > 0) && vs.styles[pdoc->StyleIndexAt(pos.Position() - 1)].IsProtected()) {
				while ((pos.Position() < pdoc->Length()) &&
					(vs.styles[pdoc->StyleIndexAt(pos.Position())].IsProtected()))
					pos.Add(1);
			}
		} else if (moveDir < 0) {
			if (vs.styles[pdoc->StyleIndexAt(pos.Position())].IsProtected()) {
				while ((pos.Position() > 0) &&
					(vs.styles[pdoc->StyleIndexAt(pos.Position() - 1)].IsProtected()))
					pos.Add(-1);
			}
		}
	}
	return pos;
}

void Editor::MovedCaret(SelectionPosition newPos, SelectionPosition previousPos,
	bool ensureVisible, CaretPolicies policies) {
	const Sci::Line currentLine = pdoc->SciLineFromPosition(newPos.Position());
	if (ensureVisible) {
		// In case in need of wrapping to ensure DisplayFromDoc works.
		if (currentLine >= wrapPending.start) {
			if (WrapLines(WrapScope::wsAll)) {
				Redraw();
			}
		}
		const XYScrollPosition newXY = XYScrollToMakeVisible(
			SelectionRange(posDrag.IsValid() ? posDrag : newPos), XYScrollOptions::all, policies);
		if (previousPos.IsValid() && (newXY.xOffset == xOffset)) {
			// simple vertical scroll then invalidate
			ScrollTo(newXY.topLine);
			InvalidateSelection(SelectionRange(previousPos), true);
		} else {
			SetXYScroll(newXY);
		}
	}

	ShowCaretAtCurrentPosition();
	NotifyCaretMove();

	ClaimSelection();
	SetHoverIndicatorPosition(sel.MainCaret());
	QueueIdleWork(WorkItems::updateUI);

	if (marginView.highlightDelimiter.NeedsDrawing(currentLine)) {
		RedrawSelMargin();
	}
}

void Editor::MovePositionTo(SelectionPosition newPos, Selection::SelTypes selt, bool ensureVisible) {
	const SelectionPosition spCaret = ((sel.Count() == 1) && sel.Empty()) ?
		sel.Last() : SelectionPosition(INVALID_POSITION);

	const Sci::Position delta = newPos.Position() - sel.MainCaret();
	newPos = ClampPositionIntoDocument(newPos);
	newPos = MovePositionOutsideChar(newPos, delta);
	if (!multipleSelection && sel.IsRectangular() && (selt == Selection::SelTypes::stream)) {
		// Can't turn into multiple selection so clear additional selections
		InvalidateSelection(SelectionRange(newPos), true);
		sel.DropAdditionalRanges();
	}
	if (!sel.IsRectangular() && (selt == Selection::SelTypes::rectangle)) {
		// Switching to rectangular
		InvalidateSelection(sel.RangeMain(), false);
		SelectionRange rangeMain = sel.RangeMain();
		sel.Clear();
		sel.Rectangular() = rangeMain;
	}
	if (selt != Selection::SelTypes::none) {
		sel.selType = selt;
	}
	if (selt != Selection::SelTypes::none || sel.MoveExtends()) {
		SetSelection(newPos);
	} else {
		SetEmptySelection(newPos);
	}

	MovedCaret(newPos, spCaret, ensureVisible, caretPolicies);
}

void Editor::MovePositionTo(Sci::Position newPos, Selection::SelTypes selt, bool ensureVisible) {
	MovePositionTo(SelectionPosition(newPos), selt, ensureVisible);
}

SelectionPosition Editor::MovePositionSoVisible(SelectionPosition pos, int moveDir) noexcept {
	pos = ClampPositionIntoDocument(pos);
	pos = MovePositionOutsideChar(pos, moveDir);
	const Sci::Line lineDoc = pdoc->SciLineFromPosition(pos.Position());
	if (pcs->GetVisible(lineDoc)) {
		return pos;
	} else {
		Sci::Line lineDisplay = pcs->DisplayFromDoc(lineDoc);
		if (moveDir > 0) {
			// lineDisplay is already line before fold as lines in fold use display line of line after fold
			lineDisplay = std::clamp<Sci::Line>(lineDisplay, 0, pcs->LinesDisplayed());
			return SelectionPosition(
				pdoc->LineStart(pcs->DocFromDisplay(lineDisplay)));
		} else {
			lineDisplay = std::clamp<Sci::Line>(lineDisplay - 1, 0, pcs->LinesDisplayed());
			return SelectionPosition(
				pdoc->LineEnd(pcs->DocFromDisplay(lineDisplay)));
		}
	}
}

SelectionPosition Editor::MovePositionSoVisible(Sci::Position pos, int moveDir) noexcept {
	return MovePositionSoVisible(SelectionPosition(pos), moveDir);
}

Point Editor::PointMainCaret() {
	return LocationFromPosition(sel.Range(sel.Main()).caret);
}

/**
 * Choose the x position that the caret will try to stick to
 * as it moves up and down.
 */
void Editor::SetLastXChosen() {
	const Point pt = PointMainCaret();
	lastXChosen = static_cast<int>(pt.x) + xOffset;
}

void Editor::ScrollTo(Sci::Line line, bool moveThumb) {
	const Sci::Line topLineNew = std::clamp<Sci::Line>(line, 0, MaxScrollPos());
	if (topLineNew != topLine) {
		// Try to optimise small scrolls
#ifndef UNDER_CE
		const Sci::Line linesToMove = topLine - topLineNew;
		const bool performBlit = (std::abs(linesToMove) <= 10) && (paintState == PaintState::notPainting);
		willRedrawAll = !performBlit;
#endif
		SetTopLine(topLineNew);
		// Optimize by styling the view as this will invalidate any needed area
		// which could abort the initial paint if discovered later.
		StyleAreaBounded(GetClientRectangle(), true);
#ifndef UNDER_CE
		// Perform redraw rather than scroll if many lines would be redrawn anyway.
		if (performBlit) {
			ScrollText(linesToMove);
		} else {
			Redraw();
		}
		willRedrawAll = false;
#else
		Redraw();
#endif
		if (moveThumb) {
			SetVerticalScrollPos();
		}
	}
}

void Editor::ScrollText(Sci::Line /* linesToMove */) {
	//Platform::DebugPrintf("Editor::ScrollText %d\n", linesToMove);
	Redraw();
}

void Editor::HorizontalScrollTo(int xPos) {
	//Platform::DebugPrintf("HorizontalScroll %d\n", xPos);
	if (xPos < 0)
		xPos = 0;
	if (!Wrapping() && (xOffset != xPos)) {
		xOffset = xPos;
		ContainerNeedsUpdate(SC_UPDATE_H_SCROLL);
		SetHorizontalScrollPos();
		RedrawRect(GetClientRectangle());
	}
}

void Editor::VerticalCentreCaret() noexcept {
	const Sci::Line lineDoc =
		pdoc->SciLineFromPosition(sel.IsRectangular() ? sel.Rectangular().caret.Position() : sel.MainCaret());
	const Sci::Line lineDisplay = pcs->DisplayFromDoc(lineDoc);
	const Sci::Line newTop = lineDisplay - (LinesOnScreen() / 2);
	if (topLine != newTop) {
		SetTopLine(newTop > 0 ? newTop : 0);
		RedrawRect(GetClientRectangle());
	}
}

void Editor::MoveSelectedLines(int lineDelta) {

	if (sel.IsRectangular()) {
		return;
	}

	// if selection doesn't start at the beginning of the line, set the new start
	Sci::Position selectionStart = SelectionStart().Position();
	const Sci::Line startLine = pdoc->SciLineFromPosition(selectionStart);
	const Sci::Position beginningOfStartLine = pdoc->LineStart(startLine);
	selectionStart = beginningOfStartLine;

	// if selection doesn't end at the beginning of a line greater than that of the start,
	// then set it at the beginning of the next one
	Sci::Position selectionEnd = SelectionEnd().Position();
	const Sci::Line endLine = pdoc->SciLineFromPosition(selectionEnd);
	const Sci::Position beginningOfEndLine = pdoc->LineStart(endLine);
	bool appendEol = false;
	if (selectionEnd > beginningOfEndLine
		|| selectionStart == selectionEnd) {
		selectionEnd = pdoc->LineStart(endLine + 1);
		appendEol = (selectionEnd == pdoc->Length() && pdoc->SciLineFromPosition(selectionEnd) == endLine);
	}

	// if there's nowhere for the selection to move
	// (i.e. at the beginning going up or at the end going down),
	// stop it right there!
	if ((selectionStart == 0 && lineDelta < 0)
		|| (selectionEnd == pdoc->Length() && lineDelta > 0)
		|| selectionStart == selectionEnd) {
		return;
	}

	UndoGroup ug(pdoc);

	if (lineDelta > 0 && selectionEnd == pdoc->LineStart(pdoc->LinesTotal() - 1)) {
		SetSelection(pdoc->MovePositionOutsideChar(selectionEnd - 1, -1), selectionEnd);
		ClearSelection();
		selectionEnd = CurrentPosition();
	}
	SetSelection(selectionStart, selectionEnd);

	SelectionText selectedText;
	CopySelectionRange(&selectedText);

	const Point currentLocation = LocationFromPosition(CurrentPosition());
	const Sci::Line currentLine = LineFromLocation(currentLocation);

	if (appendEol)
		SetSelection(pdoc->MovePositionOutsideChar(selectionStart - 1, -1), selectionEnd);
	ClearSelection();

	const char *eol = StringFromEOLMode(pdoc->eolMode);
	if (currentLine + lineDelta >= pdoc->LinesTotal())
		pdoc->InsertString(pdoc->Length(), eol, strlen(eol));
	GoToLine(currentLine + lineDelta);

	Sci::Position selectionLength = pdoc->InsertString(CurrentPosition(), selectedText.Data(), selectedText.Length());
	if (appendEol) {
		const Sci::Position lengthInserted = pdoc->InsertString(CurrentPosition() + selectionLength, eol, strlen(eol));
		selectionLength += lengthInserted;
	}
	SetSelection(CurrentPosition(), CurrentPosition() + selectionLength);
}

void Editor::MoveSelectedLinesUp() {
	MoveSelectedLines(-1);
}

void Editor::MoveSelectedLinesDown() {
	MoveSelectedLines(1);
}

void Editor::MoveCaretInsideView(bool ensureVisible) {
	const PRectangle rcClient = GetTextRectangle();
	const Point pt = PointMainCaret();
	if (pt.y < rcClient.top) {
		MovePositionTo(SPositionFromLocation(
			Point::FromInts(lastXChosen - xOffset, static_cast<int>(rcClient.top)),
			false, false, UserVirtualSpace()),
			Selection::SelTypes::none, ensureVisible);
	} else if ((pt.y + vs.lineHeight - 1) > rcClient.bottom) {
		const ptrdiff_t yOfLastLineFullyDisplayed = static_cast<ptrdiff_t>(rcClient.top) + (LinesOnScreen() - 1) * vs.lineHeight;
		MovePositionTo(SPositionFromLocation(
			Point::FromInts(lastXChosen - xOffset, static_cast<int>(rcClient.top + yOfLastLineFullyDisplayed)),
			false, false, UserVirtualSpace()),
			Selection::SelTypes::none, ensureVisible);
	}
}

Sci::Line Editor::DisplayFromPosition(Sci::Position pos) {
	AutoSurface surface(this);
	return view.DisplayFromPosition(surface, *this, pos, vs);
}

/**
 * Ensure the caret is reasonably visible in context.
 *
Caret policy in Scintilla

If slop is set, we can define a slop value.
This value defines an unwanted zone (UZ) where the caret is... unwanted.
This zone is defined as a number of pixels near the vertical margins,
and as a number of lines near the horizontal margins.
By keeping the caret away from the edges, it is seen within its context,
so it is likely that the identifier that the caret is on can be completely seen,
and that the current line is seen with some of the lines following it which are
often dependent on that line.

If strict is set, the policy is enforced... strictly.
The caret is centred on the display if slop is not set,
and cannot go in the UZ if slop is set.

If jumps is set, the display is moved more energetically
so the caret can move in the same direction longer before the policy is applied again.
'3UZ' notation is used to indicate three time the size of the UZ as a distance to the margin.

If even is not set, instead of having symmetrical UZs,
the left and bottom UZs are extended up to right and top UZs respectively.
This way, we favour the displaying of useful information: the beginning of lines,
where most code reside, and the lines after the caret, eg. the body of a function.

     |        |       |      |                                            |
slop | strict | jumps | even | Caret can go to the margin                 | When reaching limit (caret going out of
     |        |       |      |                                            | visibility or going into the UZ) display is...
-----+--------+-------+------+--------------------------------------------+--------------------------------------------------------------
  0  |   0    |   0   |   0  | Yes                                        | moved to put caret on top/on right
  0  |   0    |   0   |   1  | Yes                                        | moved by one position
  0  |   0    |   1   |   0  | Yes                                        | moved to put caret on top/on right
  0  |   0    |   1   |   1  | Yes                                        | centred on the caret
  0  |   1    |   -   |   0  | Caret is always on top/on right of display | -
  0  |   1    |   -   |   1  | No, caret is always centred                | -
  1  |   0    |   0   |   0  | Yes                                        | moved to put caret out of the asymmetrical UZ
  1  |   0    |   0   |   1  | Yes                                        | moved to put caret out of the UZ
  1  |   0    |   1   |   0  | Yes                                        | moved to put caret at 3UZ of the top or right margin
  1  |   0    |   1   |   1  | Yes                                        | moved to put caret at 3UZ of the margin
  1  |   1    |   -   |   0  | Caret is always at UZ of top/right margin  | -
  1  |   1    |   0   |   1  | No, kept out of UZ                         | moved by one position
  1  |   1    |   1   |   1  | No, kept out of UZ                         | moved to put caret at 3UZ of the margin
*/

Editor::XYScrollPosition Editor::XYScrollToMakeVisible(SelectionRange range, const XYScrollOptions options, CaretPolicies policies) {
	const PRectangle rcClient = GetTextRectangle();
	const Point ptOrigin = GetVisibleOriginInMain();
	const Point pt = LocationFromPosition(range.caret) + ptOrigin;
	const Point ptAnchor = LocationFromPosition(range.anchor) + ptOrigin;
	const Point ptBottomCaret(pt.x, pt.y + vs.lineHeight - 1);

	XYScrollPosition newXY(xOffset, topLine);
	if (rcClient.Empty()) {
		return newXY;
	}

	// Vertical positioning
	if (FlagSet(options, XYScrollOptions::vertical)
		&& (pt.y < rcClient.top || ptBottomCaret.y >= rcClient.bottom || (policies.y.policy & CARET_STRICT) != 0)) {
		const Sci::Line lineCaret = DisplayFromPosition(range.caret.Position());
		const Sci::Line linesOnScreen = LinesOnScreen();
		const Sci::Line halfScreen = std::max<Sci::Line>(linesOnScreen - 1, 2) / 2;
		const bool bSlop = (policies.y.policy & CARET_SLOP) != 0;
		const bool bStrict = (policies.y.policy & CARET_STRICT) != 0;
		const bool bJump = (policies.y.policy & CARET_JUMPS) != 0;
		const bool bEven = (policies.y.policy & CARET_EVEN) != 0;

		// It should be possible to scroll the window to show the caret,
		// but this fails to remove the caret on GTK
		if (bSlop) {	// A margin is defined
			Sci::Line yMoveT;
			Sci::Line yMoveB;
			if (bStrict) {
				Sci::Line yMarginT;
				Sci::Line yMarginB;
				if (!FlagSet(options, XYScrollOptions::useMargin)) {
					// In drag mode, avoid moves
					// otherwise, a double click will select several lines.
					yMarginT = yMarginB = 0;
				} else {
					// yMarginT must equal to caretYSlop, with a minimum of 1 and
					// a maximum of slightly less than half the height of the text area.
					yMarginT = std::clamp<Sci::Line>(policies.y.slop, 1, halfScreen);
					if (bEven) {
						yMarginB = yMarginT;
					} else {
						yMarginB = linesOnScreen - yMarginT - 1;
					}
				}
				yMoveT = yMarginT;
				if (bEven) {
					if (bJump) {
						yMoveT = std::clamp<Sci::Line>(policies.y.slop * 3, 1, halfScreen);
					}
					yMoveB = yMoveT;
				} else {
					yMoveB = linesOnScreen - yMoveT - 1;
				}
				if (lineCaret < topLine + yMarginT) {
					// Caret goes too high
					newXY.topLine = lineCaret - yMoveT;
				} else if (lineCaret > topLine + linesOnScreen - 1 - yMarginB) {
					// Caret goes too low
					newXY.topLine = lineCaret - linesOnScreen + 1 + yMoveB;
				}
			} else {	// Not strict
				yMoveT = bJump ? policies.y.slop * 3 : policies.y.slop;
				yMoveT = std::clamp<Sci::Line>(yMoveT, 1, halfScreen);
				if (bEven) {
					yMoveB = yMoveT;
				} else {
					yMoveB = linesOnScreen - yMoveT - 1;
				}
				if (lineCaret < topLine) {
					// Caret goes too high
					newXY.topLine = lineCaret - yMoveT;
				} else if (lineCaret > topLine + linesOnScreen - 1) {
					// Caret goes too low
					newXY.topLine = lineCaret - linesOnScreen + 1 + yMoveB;
				}
			}
		} else {	// No slop
			if (!bStrict && !bJump) {
				// Minimal move
				if (lineCaret < topLine) {
					// Caret goes too high
					newXY.topLine = lineCaret;
				} else if (lineCaret > topLine + linesOnScreen - 1) {
					// Caret goes too low
					if (bEven) {
						newXY.topLine = lineCaret - linesOnScreen + 1;
					} else {
						newXY.topLine = lineCaret;
					}
				}
			} else {	// Strict or going out of display
				if (bEven) {
					// Always centre caret
					newXY.topLine = lineCaret - halfScreen;
				} else {
					// Always put caret on top of display
					newXY.topLine = lineCaret;
				}
			}
		}
		if (!(range.caret == range.anchor)) {
			const Sci::Line lineAnchor = DisplayFromPosition(range.anchor.Position());
			if (lineAnchor < lineCaret) {
				// Shift up to show anchor or as much of range as possible
				newXY.topLine = std::min(newXY.topLine, lineAnchor);
				newXY.topLine = std::max(newXY.topLine, lineCaret - LinesOnScreen());
			} else {
				// Shift down to show anchor or as much of range as possible
				newXY.topLine = std::max(newXY.topLine, lineAnchor - LinesOnScreen());
				newXY.topLine = std::min(newXY.topLine, lineCaret);
			}
		}
		newXY.topLine = std::clamp<Sci::Line>(newXY.topLine, 0, MaxScrollPos());
	}

	// Horizontal positioning
	if (FlagSet(options, XYScrollOptions::horizontal) && !Wrapping()) {
		const int halfScreen = std::max(static_cast<int>(rcClient.Width()) - 4, 4) / 2;
		const bool bSlop = (policies.x.policy & CARET_SLOP) != 0;
		const bool bStrict = (policies.x.policy & CARET_STRICT) != 0;
		const bool bJump = (policies.x.policy & CARET_JUMPS) != 0;
		const bool bEven = (policies.x.policy & CARET_EVEN) != 0;

		if (bSlop) {	// A margin is defined
			int xMoveL;
			int xMoveR;
			if (bStrict) {
				int xMarginL;
				int xMarginR;
				if (!FlagSet(options, XYScrollOptions::useMargin)) {
					// In drag mode, avoid moves unless very near of the margin
					// otherwise, a simple click will select text.
					xMarginL = xMarginR = 2;
				} else {
					// xMargin must equal to caretXSlop, with a minimum of 2 and
					// a maximum of slightly less than half the width of the text area.
					xMarginR = std::clamp(policies.x.slop, 2, halfScreen);
					if (bEven) {
						xMarginL = xMarginR;
					} else {
						xMarginL = static_cast<int>(rcClient.Width()) - xMarginR - 4;
					}
				}
				if (bJump && bEven) {
					// Jump is used only in even mode
					xMoveL = xMoveR = std::clamp(policies.x.slop * 3, 1, halfScreen);
				} else {
					xMoveL = xMoveR = 0;	// Not used, avoid a warning
				}
				if (pt.x < rcClient.left + xMarginL) {
					// Caret is on the left of the display
					if (bJump && bEven) {
						newXY.xOffset -= xMoveL;
					} else {
						// Move just enough to allow to display the caret
						newXY.xOffset -= static_cast<int>((rcClient.left + xMarginL) - pt.x);
					}
				} else if (pt.x >= rcClient.right - xMarginR) {
					// Caret is on the right of the display
					if (bJump && bEven) {
						newXY.xOffset += xMoveR;
					} else {
						// Move just enough to allow to display the caret
						newXY.xOffset += static_cast<int>(pt.x - (rcClient.right - xMarginR) + 1);
					}
				}
			} else {	// Not strict
				xMoveR = bJump ? policies.x.slop * 3 : policies.x.slop;
				xMoveR = std::clamp(xMoveR, 1, halfScreen);
				if (bEven) {
					xMoveL = xMoveR;
				} else {
					xMoveL = static_cast<int>(rcClient.Width()) - xMoveR - 4;
				}
				if (pt.x < rcClient.left) {
					// Caret is on the left of the display
					newXY.xOffset -= xMoveL;
				} else if (pt.x >= rcClient.right) {
					// Caret is on the right of the display
					newXY.xOffset += xMoveR;
				}
			}
		} else {	// No slop
			if (bStrict ||
				(bJump && (pt.x < rcClient.left || pt.x >= rcClient.right))) {
				// Strict or going out of display
				if (bEven) {
					// centre caret
					newXY.xOffset += static_cast<int>(pt.x - rcClient.left - halfScreen);
				} else {
					// Put caret on right
					newXY.xOffset += static_cast<int>(pt.x - rcClient.right + 1);
				}
			} else {
				// Move just enough to allow to display the caret
				if (pt.x < rcClient.left) {
					// Caret is on the left of the display
					if (bEven) {
						newXY.xOffset -= static_cast<int>(rcClient.left - pt.x);
					} else {
						newXY.xOffset += static_cast<int>(pt.x - rcClient.right) + 1;
					}
				} else if (pt.x >= rcClient.right) {
					// Caret is on the right of the display
					newXY.xOffset += static_cast<int>(pt.x - rcClient.right) + 1;
				}
			}
		}
		// In case of a jump (find result) largely out of display, adjust the offset to display the caret
		if (pt.x + xOffset < rcClient.left + newXY.xOffset) {
			newXY.xOffset = static_cast<int>(pt.x + xOffset - rcClient.left) - 2;
		} else if (pt.x + xOffset >= rcClient.right + newXY.xOffset) {
			newXY.xOffset = static_cast<int>(pt.x + xOffset - rcClient.right) + 2;
			if (vs.IsBlockCaretStyle() || view.imeCaretBlockOverride) {
				// Ensure we can see a good portion of the block caret
				newXY.xOffset += static_cast<int>(vs.aveCharWidth);
			}
		}
		if (!(range.caret == range.anchor)) {
			if (ptAnchor.x < pt.x) {
				// Shift to left to show anchor or as much of range as possible
				const int maxOffset = static_cast<int>(ptAnchor.x + xOffset - rcClient.left) - 1;
				const int minOffset = static_cast<int>(pt.x + xOffset - rcClient.right) + 1;
				newXY.xOffset = std::min(newXY.xOffset, maxOffset);
				newXY.xOffset = std::max(newXY.xOffset, minOffset);
			} else {
				// Shift to right to show anchor or as much of range as possible
				const int minOffset = static_cast<int>(ptAnchor.x + xOffset - rcClient.right) + 1;
				const int maxOffset = static_cast<int>(pt.x + xOffset - rcClient.left) - 1;
				newXY.xOffset = std::max(newXY.xOffset, minOffset);
				newXY.xOffset = std::min(newXY.xOffset, maxOffset);
			}
		}
		if (newXY.xOffset < 0) {
			newXY.xOffset = 0;
		}
	}

	return newXY;
}

void Editor::SetXYScroll(XYScrollPosition newXY) {
	if ((newXY.topLine != topLine) || (newXY.xOffset != xOffset)) {
		if (newXY.topLine != topLine) {
			SetTopLine(newXY.topLine);
			SetVerticalScrollPos();
		}
		if (newXY.xOffset != xOffset) {
			xOffset = newXY.xOffset;
			ContainerNeedsUpdate(SC_UPDATE_H_SCROLL);
			if (newXY.xOffset > 0) {
				const PRectangle rcText = GetTextRectangle();
				if (horizontalScrollBarVisible &&
					rcText.Width() + xOffset > scrollWidth) {
					scrollWidth = xOffset + static_cast<int>(rcText.Width());
					SetScrollBars();
				}
			}
			SetHorizontalScrollPos();
		}
		Redraw();
		UpdateSystemCaret();
	}
}

void Editor::ScrollRange(SelectionRange range) {
	SetXYScroll(XYScrollToMakeVisible(range, XYScrollOptions::all, caretPolicies));
}

namespace {

constexpr XYScrollOptions operator|(XYScrollOptions a, XYScrollOptions b) noexcept {
	return static_cast<XYScrollOptions>(static_cast<int>(a) | static_cast<int>(b));
}

}

void Editor::EnsureCaretVisible(bool useMargin, bool vert, bool horiz) {
	SetXYScroll(XYScrollToMakeVisible(SelectionRange(posDrag.IsValid() ? posDrag : sel.RangeMain().caret),
		(useMargin ? XYScrollOptions::useMargin : XYScrollOptions::none) |
		(vert ? XYScrollOptions::vertical : XYScrollOptions::none) |
		(horiz ? XYScrollOptions::horizontal : XYScrollOptions::none),
		caretPolicies));
}

void Editor::ShowCaretAtCurrentPosition() {
	if (hasFocus) {
		caret.active = true;
		caret.on = true;
		FineTickerCancel(TickReason::caret);
		if (caret.period > 0)
			FineTickerStart(TickReason::caret, caret.period, caret.period / 10);
	} else {
		caret.active = false;
		caret.on = false;
		FineTickerCancel(TickReason::caret);
	}
	InvalidateCaret();
}

void Editor::DropCaret() {
	caret.active = false;
	FineTickerCancel(TickReason::caret);
	InvalidateCaret();
}

void Editor::CaretSetPeriod(int period) {
	if (caret.period != period) {
		caret.period = period;
		caret.on = true;
		FineTickerCancel(TickReason::caret);
		if ((caret.active) && (caret.period > 0))
			FineTickerStart(TickReason::caret, caret.period, caret.period / 10);
		InvalidateCaret();
	}
}

void Editor::InvalidateCaret() {
	if (posDrag.IsValid()) {
		InvalidateRange(posDrag.Position(), posDrag.Position() + 1);
	} else {
		for (size_t r = 0; r < sel.Count(); r++) {
			InvalidateRange(sel.Range(r).caret.Position(), sel.Range(r).caret.Position() + 1);
		}
	}
	UpdateSystemCaret();
}

void Editor::NotifyCaretMove() noexcept {
}

void Editor::UpdateSystemCaret() {
}

bool Editor::Wrapping() const noexcept {
	return vs.wrap.state != WrapMode::none;
}

void Editor::NeedWrapping(Sci::Line docLineStart, Sci::Line docLineEnd) noexcept {
	//Platform::DebugPrintf("\nNeedWrapping: %0d..%0d\n", docLineStart, docLineEnd);
	if (wrapPending.AddRange(docLineStart, docLineEnd)) {
		view.llc.Invalidate(LineLayout::ValidLevel::positions);
	}
	// Wrap lines during idle.
	if (Wrapping() && wrapPending.NeedsWrap()) {
		SetIdle(true);
	}
}

bool Editor::WrapOneLine(Surface *surface, Sci::Line lineToWrap) {
	LineLayout *ll = view.RetrieveLineLayout(lineToWrap, *this);
	int linesWrapped = 1;
	if (ll) {
		view.LayoutLine(*this, surface, vs, ll, wrapWidth);
		linesWrapped = ll->lines;
	}
	return pcs->SetHeight(lineToWrap, linesWrapped +
		(vs.annotationVisible ? pdoc->AnnotationLines(lineToWrap) : 0));
}

// Perform  wrapping for a subset of the lines needing wrapping.
// wsAll: wrap all lines which need wrapping in this single call
// wsVisible: wrap currently visible lines
// wsIdle: wrap one page + 100 lines
// Return true if wrapping occurred.
bool Editor::WrapLines(WrapScope ws) {
//#define WRAP_LINES_TIMING
#ifdef WRAP_LINES_TIMING
	struct WrapTiming {
		size_t totalBytes;
		size_t totalRuns;
		double totalDuration;
		using Clock = std::chrono::steady_clock;
		Clock::time_point startTime;

		void Reset() noexcept {
			totalBytes = 0;
			totalRuns = 0;
			totalDuration = 0;
		}
		void Add(size_t bytes, double duration) noexcept {
			totalBytes += bytes;
			++totalRuns;
			totalDuration += duration;
		}
		double TotalTime() const noexcept {
			const auto tpNow = Clock::now();
			const auto duration = std::chrono::duration_cast<std::chrono::duration<double>>(tpNow - startTime);
			return duration.count();
		}
	};

	static WrapTiming wrapTiming;
#endif
	Sci::Line goodTopLine = topLine;
	bool wrapOccurred = false;
	if (!Wrapping()) {
		if (wrapWidth != LineLayout::wrapWidthInfinite) {
			wrapWidth = LineLayout::wrapWidthInfinite;
			for (Sci::Line lineDoc = 0; lineDoc < pdoc->LinesTotal(); lineDoc++) {
				pcs->SetHeight(lineDoc, 1 +
					(vs.annotationVisible ? pdoc->AnnotationLines(lineDoc) : 0));
			}
			wrapOccurred = true;
		}
		wrapPending.Reset();
	} else if (wrapPending.NeedsWrap()) {
		wrapPending.start = std::min(wrapPending.start, pdoc->LinesTotal());
		if (!SetIdle(true)) {
			// Idle processing not supported so full wrap required.
			ws = WrapScope::wsAll;
		}
		// Decide where to start wrapping
		Sci::Line lineToWrap = wrapPending.start;
#ifdef WRAP_LINES_TIMING
		if (lineToWrap == 0) {
			printf("%s wrap start ws=%d, modEventMask=%x\n", __func__, ws, modEventMask);
			wrapTiming.Reset();
			wrapTiming.startTime = WrapTiming::Clock::now();
		}
#endif
		Sci::Line lineToWrapEnd = std::min(wrapPending.end, pdoc->LinesTotal());
		const Sci::Line lineDocTop = pcs->DocFromDisplay(topLine);
		const Sci::Line subLineTop = topLine - pcs->DisplayFromDoc(lineDocTop);
		if (ws == WrapScope::wsVisible) {
			lineToWrap = std::clamp(lineDocTop - 5, wrapPending.start, pdoc->LinesTotal());
			// Priority wrap to just after visible area.
			// Since wrapping could reduce display lines, treat each
			// as taking only one display line.
			lineToWrapEnd = lineDocTop;
			Sci::Line lines = LinesOnScreen() + 1;
			const Sci::Line lineLast = pdoc->LineFromPositionAfter(lineToWrap, ActionDuration::InitialBytes);
			const Sci::Line maxLine = std::min(lineLast, pcs->LinesInDoc());
			while ((lineToWrapEnd < maxLine) && (lines > 0)) {
				if (pcs->GetVisible(lineToWrapEnd))
					lines--;
				lineToWrapEnd++;
			}
			// .. and if the paint window is outside pending wraps
			if ((lineToWrap > wrapPending.end) || (lineToWrapEnd < wrapPending.start)) {
				// Currently visible text does not need wrapping
				return false;
			}
		} else if (ws == WrapScope::wsIdle) {
			// Try to keep time taken by wrapping reasonable so interaction remains smooth.
			constexpr double secondsAllowed = 0.01;
			const Sci::Position actionsInAllowedTime = durationWrapOneUnit.ActionsInAllowedTime(secondsAllowed);
			lineToWrapEnd = pdoc->LineFromPositionAfter(lineToWrap, actionsInAllowedTime);
		}
		const Sci::Line lineEndNeedWrap = std::min(wrapPending.end, pdoc->LinesTotal());
		lineToWrapEnd = std::min(lineToWrapEnd, lineEndNeedWrap);

		// Ensure all lines being wrapped are styled.
		pdoc->EnsureStyledTo(pdoc->LineStart(lineToWrapEnd));

		if (lineToWrap < lineToWrapEnd) {

			PRectangle rcTextArea = GetClientRectangle();
			rcTextArea.left = static_cast<XYPOSITION>(vs.textStart);
			rcTextArea.right -= vs.rightMarginWidth;
			wrapWidth = static_cast<int>(rcTextArea.Width());
			RefreshStyleData();
			AutoSurface surface(this);
			if (surface) {
				//Platform::DebugPrintf("Wraplines: scope=%0d need=%0d..%0d perform=%0d..%0d\n", ws, wrapPending.start, wrapPending.end, lineToWrap, lineToWrapEnd);
				const Sci::Position bytesBeingWrapped = pdoc->LineStart(lineToWrapEnd) - pdoc->LineStart(lineToWrap);
				const ElapsedPeriod epWrapping;
				while (lineToWrap < lineToWrapEnd) {
					if (WrapOneLine(surface, lineToWrap)) {
						wrapOccurred = true;
					}
					wrapPending.Wrapped(lineToWrap);
					lineToWrap++;
				}
				const double duration = epWrapping.Duration();
#ifdef WRAP_LINES_TIMING
				wrapTiming.Add(bytesBeingWrapped, duration);
#endif
				durationWrapOneUnit.AddSample(bytesBeingWrapped, duration);

				goodTopLine = pcs->DisplayFromDoc(lineDocTop) + std::min(
					subLineTop, static_cast<Sci::Line>(pcs->GetHeight(lineDocTop) - 1));
			}
		}

		// If wrapping is done, bring it to resting position
		if (wrapPending.start >= lineEndNeedWrap) {
			wrapPending.Reset();
#ifdef WRAP_LINES_TIMING
			const double totalTime = wrapTiming.TotalTime();
			printf("%s bytes=%zu / %zu, time=%.6f / %.6f, level=%d,\n"
				"\tLinesOnScreen=%zd, idleStyling=%d, modEventMask=%x, technology=%d, dbcsCodePage=%d\n",
				__func__, wrapTiming.totalBytes, wrapTiming.totalRuns, wrapTiming.totalDuration, totalTime,
				view.llc.GetLevel(), LinesOnScreen(), idleStyling, modEventMask, technology, pdoc->dbcsCodePage);
			wrapTiming.Reset();
#endif
		}
	}

	if (wrapOccurred) {
		SetScrollBars();
		SetTopLine(std::clamp<Sci::Line>(goodTopLine, 0, MaxScrollPos()));
		SetVerticalScrollPos();
	}

	return wrapOccurred;
}

void Editor::LinesJoin() {
	if (!RangeContainsProtected(targetRange.start.Position(), targetRange.end.Position())) {
		UndoGroup ug(pdoc);
		bool prevNonWS = true;
		for (Sci::Position pos = targetRange.start.Position(); pos < targetRange.end.Position(); pos++) {
			if (pdoc->IsPositionInLineEnd(pos)) {
				targetRange.end.Add(-pdoc->LenChar(pos));
				pdoc->DelChar(pos);
				if (prevNonWS) {
					// Ensure at least one space separating previous lines
					const Sci::Position lengthInserted = pdoc->InsertString(pos, " ", 1);
					targetRange.end.Add(lengthInserted);
				}
			} else {
				prevNonWS = pdoc->CharAt(pos) != ' ';
			}
		}
	}
}

const char *Editor::StringFromEOLMode(int eolMode) noexcept {
	if (eolMode == SC_EOL_CRLF) {
		return "\r\n";
	} else if (eolMode == SC_EOL_CR) {
		return "\r";
	} else {
		return "\n";
	}
}

void Editor::LinesSplit(int pixelWidth) {
	if (!RangeContainsProtected(targetRange.start.Position(), targetRange.end.Position())) {
		if (pixelWidth == 0) {
			const PRectangle rcText = GetTextRectangle();
			pixelWidth = static_cast<int>(rcText.Width());
		}
		const Sci::Line lineStart = pdoc->SciLineFromPosition(targetRange.start.Position());
		Sci::Line lineEnd = pdoc->SciLineFromPosition(targetRange.end.Position());
		const char *eol = StringFromEOLMode(pdoc->eolMode);
		UndoGroup ug(pdoc);
		for (Sci::Line line = lineStart; line <= lineEnd; line++) {
			AutoSurface surface(this);
			LineLayout *ll = view.RetrieveLineLayout(line, *this);
			if (surface && ll) {
				const Sci::Position posLineStart = pdoc->LineStart(line);
				view.LayoutLine(*this, surface, vs, ll, pixelWidth);
				Sci::Position lengthInsertedTotal = 0;
				for (int subLine = 1; subLine < ll->lines; subLine++) {
					const Sci::Position lengthInserted = pdoc->InsertString(
						posLineStart + lengthInsertedTotal + ll->LineStart(subLine),
						eol, strlen(eol));
					targetRange.end.Add(lengthInserted);
					lengthInsertedTotal += lengthInserted;
				}
			}
			lineEnd = pdoc->SciLineFromPosition(targetRange.end.Position());
		}
	}
}

void Editor::PaintSelMargin(Surface *surfaceWindow, PRectangle rc) {
	if (vs.fixedColumnWidth == 0)
		return;

	RefreshStyleData();
	RefreshPixMaps(surfaceWindow);

	// On GTK with Ubuntu overlay scroll bars, the surface may have been finished
	// at this point. The Initialised call checks for this case and sets the status
	// to be bad which avoids crashes in following calls.
	if (!surfaceWindow->Initialised()) {
		return;
	}

	PRectangle rcMargin = GetClientRectangle();
	const Point ptOrigin = GetVisibleOriginInMain();
	rcMargin.Move(0, -ptOrigin.y);
	rcMargin.left = 0;
	rcMargin.right = static_cast<XYPOSITION>(vs.fixedColumnWidth);

	if (!rc.Intersects(rcMargin))
		return;

	Surface *surface;
	if (view.bufferedDraw) {
		surface = marginView.pixmapSelMargin.get();
	} else {
		surface = surfaceWindow;
	}

	// Clip vertically to paint area to avoid drawing line numbers
	rcMargin.bottom = std::min(rcMargin.bottom, rc.bottom);
	rcMargin.top = std::max(rcMargin.top, rc.top);

	marginView.PaintMargin(surface, topLine, rc, rcMargin, *this, vs);

	if (view.bufferedDraw) {
		marginView.pixmapSelMargin->FlushDrawing();
		surfaceWindow->Copy(rcMargin, Point(rcMargin.left, rcMargin.top), *marginView.pixmapSelMargin);
	}
}

void Editor::RefreshPixMaps(Surface *surfaceWindow) {
	view.RefreshPixMaps(surfaceWindow, vs);
	marginView.RefreshPixMaps(surfaceWindow, vs);
	if (view.bufferedDraw) {
		const PRectangle rcClient = GetClientRectangle();
		if (!view.pixmapLine) {
			view.pixmapLine = surfaceWindow->AllocatePixMap(static_cast<int>(rcClient.Width()), vs.lineHeight);
		}
		if (!marginView.pixmapSelMargin) {
			marginView.pixmapSelMargin = surfaceWindow->AllocatePixMap(vs.fixedColumnWidth,
				static_cast<int>(rcClient.Height()));
		}
	}
}

void Editor::Paint(Surface *surfaceWindow, PRectangle rcArea) {
	//Platform::DebugPrintf("Paint:%1d (%.0f,%.0f) ... (%.0f,%.0f)\n",
	//	paintingAllText, rcArea.left, rcArea.top, rcArea.right, rcArea.bottom);

	RefreshStyleData();
	if (paintState == PaintState::abandoned)
		return;	// Scroll bars may have changed so need redraw
	RefreshPixMaps(surfaceWindow);

	paintAbandonedByStyling = false;

	StyleAreaBounded(rcArea, false);

	const PRectangle rcClient = GetClientRectangle();
	//Platform::DebugPrintf("Client: (%.0f,%.0f) ... (%.0f,%.0f)\n",
	//	rcClient.left, rcClient.top, rcClient.right, rcClient.bottom);

	if (NotifyUpdateUI()) {
		RefreshStyleData();
		RefreshPixMaps(surfaceWindow);
	}

	// Wrap the visible lines if needed.
	if (WrapLines(WrapScope::wsVisible)) {
		// The wrapping process has changed the height of some lines so
		// abandon this paint for a complete repaint.
		if (AbandonPaint()) {
			return;
		}
		RefreshPixMaps(surfaceWindow);	// In case pixmaps invalidated by scrollbar change
	}

	if (!marginView.pixmapSelPattern->Initialised()) {
		// When Direct2D is used, pixmap creation may fail with D2DERR_RECREATE_TARGET so
		// abandon this paint to avoid further failures.
		// Main drawing surface and pixmaps should be recreated by next paint.
		return;
	}

	if (!view.bufferedDraw)
		surfaceWindow->SetClip(rcArea);

	if (paintState != PaintState::abandoned) {
		if (vs.marginInside) {
			PaintSelMargin(surfaceWindow, rcArea);
			PRectangle rcRightMargin = rcClient;
			rcRightMargin.left = rcRightMargin.right - vs.rightMarginWidth;
			if (rcArea.Intersects(rcRightMargin)) {
				surfaceWindow->FillRectangle(rcRightMargin, vs.styles[STYLE_DEFAULT].back);
			}
		} else { // Else separate view so separate paint event but leftMargin included to allow overlap
			PRectangle rcLeftMargin = rcArea;
			rcLeftMargin.left = 0;
			rcLeftMargin.right = rcLeftMargin.left + vs.leftMarginWidth;
			if (rcArea.Intersects(rcLeftMargin)) {
				surfaceWindow->FillRectangle(rcLeftMargin, vs.styles[STYLE_DEFAULT].back);
			}
		}
	}

	if (paintState == PaintState::abandoned) {
		// Either styling or NotifyUpdateUI noticed that painting is needed
		// outside the current painting rectangle
		//Platform::DebugPrintf("Abandoning paint\n");
		if (Wrapping()) {
			if (paintAbandonedByStyling) {
				// Styling has spilled over a line end, such as occurs by starting a multiline
				// comment. The width of subsequent text may have changed, so rewrap.
				NeedWrapping(pcs->DocFromDisplay(topLine));
			}
		}
		if (!view.bufferedDraw)
			surfaceWindow->PopClip();
		return;
	}

	view.PaintText(surfaceWindow, *this, rcArea, rcClient, vs);

	if (horizontalScrollBarVisible && trackLineWidth && (view.lineWidthMaxSeen > scrollWidth)) {
		scrollWidth = view.lineWidthMaxSeen;
		if (!FineTickerRunning(TickReason::widen)) {
			FineTickerStart(TickReason::widen, 50, 5);
		}
	}

	if (!view.bufferedDraw)
		surfaceWindow->PopClip();

	NotifyPainted();
}

// This is mostly copied from the Paint method but with some things omitted
// such as the margin markers, line numbers, selection and caret
// Should be merged back into a combined Draw method.
Sci::Position Editor::FormatRange(bool draw, const Sci_RangeToFormat *pfr) {
	if (!pfr)
		return 0;

	AutoSurface surface(pfr->hdc, this, SC_TECHNOLOGY_DEFAULT, true);
	if (!surface)
		return 0;
	AutoSurface surfaceMeasure(pfr->hdcTarget, this, SC_TECHNOLOGY_DEFAULT, true);
	if (!surfaceMeasure) {
		return 0;
	}
	return view.FormatRange(draw, pfr, surface, surfaceMeasure, *this, vs);
}

long Editor::TextWidth(uptr_t style, const char *text) {
	RefreshStyleData();
	AutoSurface surface(this);
	if (surface) {
		return std::lround(surface->WidthText(vs.styles[style].font.get(), text));
	} else {
		return 1;
	}
}

// Empty method is overridden on GTK to show / hide scrollbars
void Editor::ReconfigureScrollBars() noexcept {}

void Editor::SetScrollBars() {
	RefreshStyleData();

	const Sci::Line nMax = MaxScrollPos();
	const Sci::Line nPage = LinesOnScreen();
	const bool modified = ModifyScrollBars(nMax + nPage - 1, nPage);
	if (modified) {
		DwellEnd(true);
	}

	// TODO: ensure always showing as many lines as possible
	// May not be, if, for example, window made larger
	if (topLine > MaxScrollPos()) {
		SetTopLine(std::clamp<Sci::Line>(topLine, 0, MaxScrollPos()));
		SetVerticalScrollPos();
		Redraw();
	}
	if (modified) {
		if (!AbandonPaint())
			Redraw();
	}
	//Platform::DebugPrintf("end max = %d page = %d\n", nMax, nPage);
}

void Editor::ChangeSize() {
	DropGraphics();
	SetScrollBars();
	if (Wrapping()) {
		PRectangle rcTextArea = GetClientRectangle();
		rcTextArea.left = static_cast<XYPOSITION>(vs.textStart);
		rcTextArea.right -= vs.rightMarginWidth;
		if (wrapWidth != rcTextArea.Width()) {
			NeedWrapping();
			Redraw();
		}
	}
}

Sci::Position Editor::RealizeVirtualSpace(Sci::Position position, Sci::Position virtualSpace) {
	if (virtualSpace > 0) {
		const Sci::Line line = pdoc->SciLineFromPosition(position);
		const Sci::Position indent = pdoc->GetLineIndentPosition(line);
		if (indent == position) {
			return pdoc->SetLineIndentation(line, pdoc->GetLineIndentation(line) + virtualSpace);
		} else {
			std::string spaceText(virtualSpace, ' ');
			const Sci::Position lengthInserted = pdoc->InsertString(position, spaceText.c_str(), virtualSpace);
			position += lengthInserted;
		}
	}
	return position;
}

SelectionPosition Editor::RealizeVirtualSpace(SelectionPosition position) {
	// Return the new position with no virtual space
	return SelectionPosition(RealizeVirtualSpace(position.Position(), position.VirtualSpace()));
}

void Editor::AddChar(char ch) {
	const char s[2] = { ch, '\0' };
	InsertCharacter(std::string_view(s, 1), CharacterSource::directInput);
}

void Editor::FilterSelections() {
	if (!additionalSelectionTyping && (sel.Count() > 1)) {
		InvalidateWholeSelection();
		sel.DropAdditionalRanges();
	}
}

static constexpr char EncloseSelectionCharacter(char ch) noexcept {
	switch (ch) {
	case '(':
		return ')';
	case '[':
		return ']';
	case '{':
		return '}';
	//case '<':
	//	return '>';
	case '\"':
	case '\'':
	case '`':
		return ch;
	default:
		return '\0';
	}
}

// InsertCharacter inserts a character encoded in document code page.
void Editor::InsertCharacter(std::string_view sv, CharacterSource charSource) {
	if (sv.empty()) {
		return;
	}
	FilterSelections();
	bool handled = false;
	{
		UndoGroup ug(pdoc, (sel.Count() > 1) || !sel.Empty() || inOverstrike);
		// enclose selection on typing punctuation, empty selection will be handled in SCN_CHARADDED.
		const char encloseCh = (charSource != CharacterSource::directInput || sv.length() != 1
			|| sel.IsRectangular() || sel.Empty()) ? '\0' : EncloseSelectionCharacter(sv[0]);

		// Vector elements point into selection in order to change selection.
		std::vector<SelectionRange *> selPtrs;
		for (size_t r = 0; r < sel.Count(); r++) {
			selPtrs.push_back(&sel.Range(r));
		}
		// Order selections by position in document.
		std::sort(selPtrs.begin(), selPtrs.end(),
			[](const SelectionRange *a, const SelectionRange *b) noexcept { return *a < *b; });

		// Loop in reverse to avoid disturbing positions of selections yet to be processed.
		for (auto rit = selPtrs.rbegin(); rit != selPtrs.rend(); ++rit) {
			SelectionRange *currentSel = *rit;
			if (!RangeContainsProtected(currentSel->Start().Position(),
				currentSel->End().Position())) {
				Sci::Position positionInsert = currentSel->Start().Position();
				std::string text;
				bool forward = false;
				if (!currentSel->Empty()) {
					const Sci::Position selectionLength = currentSel->Length();
					if (selectionLength) {
						if (encloseCh) {
							forward = currentSel->anchor < currentSel->caret;
							text.resize(selectionLength + 2);
							text[0] = sv[0];
							pdoc->GetCharRange(text.data() + 1, positionInsert, selectionLength);
							text[selectionLength + 1] = encloseCh;
						}
						pdoc->DeleteChars(positionInsert, selectionLength);
						currentSel->ClearVirtualSpace();
					} else {
						// Range is all virtual so collapse to start of virtual space
						currentSel->MinimizeVirtualSpace();
					}
				} else if (inOverstrike) {
					if (positionInsert < pdoc->Length()) {
						if (!pdoc->IsPositionInLineEnd(positionInsert)) {
							pdoc->DelChar(positionInsert);
							currentSel->ClearVirtualSpace();
						}
					}
				}
				positionInsert = RealizeVirtualSpace(positionInsert, currentSel->caret.VirtualSpace());
				if (text.empty()) {
					const Sci::Position lengthInserted = pdoc->InsertString(positionInsert, sv.data(), sv.length());
					if (lengthInserted > 0) {
						currentSel->caret.SetPosition(positionInsert + lengthInserted);
						currentSel->anchor.SetPosition(positionInsert + lengthInserted);
					}
				} else {
					const Sci::Position lengthInserted = pdoc->InsertString(positionInsert, text.data(), text.length());
					if (lengthInserted > 0) {
						handled = true;
						if (forward) {
							currentSel->caret.SetPosition(positionInsert + lengthInserted - 1);
							currentSel->anchor.SetPosition(positionInsert + 1);
						} else {
							currentSel->caret.SetPosition(positionInsert + 1);
							currentSel->anchor.SetPosition(positionInsert + lengthInserted - 1);
						}
					}
				}
				currentSel->ClearVirtualSpace();
				// If in wrap mode rewrap current line so EnsureCaretVisible has accurate information
				if (Wrapping()) {
					AutoSurface surface(this);
					if (surface) {
						if (WrapOneLine(surface, pdoc->SciLineFromPosition(positionInsert))) {
							SetScrollBars();
							SetVerticalScrollPos();
							Redraw();
						}
					}
				}
			}
		}
	}
	if (Wrapping()) {
		SetScrollBars();
	}
	ThinRectangularRange();
	// If in wrap mode rewrap current line so EnsureCaretVisible has accurate information
	EnsureCaretVisible();
	// Avoid blinking during rapid typing:
	ShowCaretAtCurrentPosition();
	if ((caretSticky == SC_CARETSTICKY_OFF) ||
		((caretSticky == SC_CARETSTICKY_WHITESPACE) && !IsAllSpacesOrTabs(sv))) {
		SetLastXChosen();
	}

	// We don't handle inline IME tentative input characters
	if (!handled && charSource != CharacterSource::tentativeInput && sel.Count() == 1) {
		int ch = static_cast<unsigned char>(sv[0]);
		if (pdoc->dbcsCodePage != SC_CP_UTF8) {
			if (sv.length() > 1) {
				// DBCS code page or DBCS font character set.
				ch = (ch << 8) | static_cast<unsigned char>(sv[1]);
			}
		} else {
			if ((ch < 0xC2) || (1 == sv.length())) {
				// Handles UTF-8 characters between 0x01 and 0x7F and single byte
				// characters when not in UTF-8 mode.
				// Also treats \0 and naked trail bytes 0x80 to 0xBF as valid
				// characters representing themselves.
			} else {
				unsigned int utf32[1] = { 0 };
				UTF32FromUTF8(sv, utf32, std::size(utf32));
				ch = utf32[0];
			}
		}
		NotifyChar(ch, charSource);
	}

	if (recordingMacro && charSource != CharacterSource::tentativeInput) {
		std::string copy(sv); // ensure NUL-terminated
		NotifyMacroRecord(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>(copy.data()));
	}
}

void Editor::ClearBeforeTentativeStart() {
	// Make positions for the first composition string.
	FilterSelections();
	UndoGroup ug(pdoc, (sel.Count() > 1) || !sel.Empty() || inOverstrike);
	for (size_t r = 0; r < sel.Count(); r++) {
		if (!RangeContainsProtected(sel.Range(r).Start().Position(),
			sel.Range(r).End().Position())) {
			const Sci::Position positionInsert = sel.Range(r).Start().Position();
			if (!sel.Range(r).Empty()) {
				if (sel.Range(r).Length()) {
					pdoc->DeleteChars(positionInsert, sel.Range(r).Length());
					sel.Range(r).ClearVirtualSpace();
				} else {
					// Range is all virtual so collapse to start of virtual space
					sel.Range(r).MinimizeVirtualSpace();
				}
			}
			RealizeVirtualSpace(positionInsert, sel.Range(r).caret.VirtualSpace());
			sel.Range(r).ClearVirtualSpace();
		}
	}
}

void Editor::InsertPaste(const char *text, Sci::Position len) {
	if (multiPasteMode == SC_MULTIPASTE_ONCE) {
		SelectionPosition selStart = sel.Start();
		selStart = RealizeVirtualSpace(selStart);
		const Sci::Position lengthInserted = pdoc->InsertString(selStart.Position(), text, len);
		if (lengthInserted > 0) {
			SetEmptySelection(selStart.Position() + lengthInserted);
		}
	} else {
		// SC_MULTIPASTE_EACH
		for (size_t r = 0; r < sel.Count(); r++) {
			if (!RangeContainsProtected(sel.Range(r).Start().Position(),
				sel.Range(r).End().Position())) {
				Sci::Position positionInsert = sel.Range(r).Start().Position();
				if (!sel.Range(r).Empty()) {
					if (sel.Range(r).Length()) {
						pdoc->DeleteChars(positionInsert, sel.Range(r).Length());
						sel.Range(r).ClearVirtualSpace();
					} else {
						// Range is all virtual so collapse to start of virtual space
						sel.Range(r).MinimizeVirtualSpace();
					}
				}
				positionInsert = RealizeVirtualSpace(positionInsert, sel.Range(r).caret.VirtualSpace());
				const Sci::Position lengthInserted = pdoc->InsertString(positionInsert, text, len);
				if (lengthInserted > 0) {
					sel.Range(r).caret.SetPosition(positionInsert + lengthInserted);
					sel.Range(r).anchor.SetPosition(positionInsert + lengthInserted);
				}
				sel.Range(r).ClearVirtualSpace();
			}
		}
	}
}

void Editor::InsertPasteShape(const char *text, Sci::Position len, PasteShape shape) {
	std::string convertedText;
	if (convertPastes) {
		// Convert line endings of the paste into our local line-endings mode
		convertedText = Document::TransformLineEnds(text, len, pdoc->eolMode);
		len = convertedText.length();
		text = convertedText.c_str();
	}
	if (shape == PasteShape::rectangular) {
		PasteRectangular(sel.Start(), text, len);
	} else {
		if (shape == PasteShape::line) {
			const Sci::Position insertPos =
				pdoc->LineStart(pdoc->LineFromPosition(sel.MainCaret()));
			Sci::Position lengthInserted = pdoc->InsertString(insertPos, text, len);
			// add the newline if necessary
			if ((len > 0) && !IsEOLChar(text[len - 1])) {
				const char *endline = StringFromEOLMode(pdoc->eolMode);
				const Sci::Position length = strlen(endline);
				lengthInserted += pdoc->InsertString(insertPos + lengthInserted, endline, length);
			}
			if (sel.MainCaret() == insertPos) {
				SetEmptySelection(sel.MainCaret() + lengthInserted);
			}
		} else {
			InsertPaste(text, len);
		}
	}
}

void Editor::ClearSelection(bool retainMultipleSelections) {
	if (!sel.IsRectangular() && !retainMultipleSelections)
		FilterSelections();
	UndoGroup ug(pdoc);
	for (size_t r = 0; r < sel.Count(); r++) {
		if (!sel.Range(r).Empty()) {
			SelectionRange rangeNew = sel.Range(r);
			if (sel.selType == Selection::SelTypes::lines && sel.Count() == 1) {
				// remove EOLs
				rangeNew = LineSelectionRange(rangeNew.caret, rangeNew.anchor, true);
			}
			if (!RangeContainsProtected(rangeNew.Start().Position(),
				rangeNew.End().Position())) {
				pdoc->DeleteChars(rangeNew.Start().Position(),
					rangeNew.Length());
				sel.Range(r) = SelectionRange(rangeNew.Start());
			}
		}
	}
	ThinRectangularRange();
	sel.RemoveDuplicates();
	ClaimSelection();
	SetHoverIndicatorPosition(sel.MainCaret());
}

void Editor::ClearAll() {
	{
		UndoGroup ug(pdoc);
		if (0 != pdoc->Length()) {
			pdoc->DeleteChars(0, pdoc->Length());
		}
		if (!pdoc->IsReadOnly()) {
			pcs->Clear();
			pdoc->AnnotationClearAll();
			pdoc->EOLAnnotationClearAll();
			pdoc->MarginClearAll();
		}
	}

	view.ClearAllTabstops();

	sel.Clear();
	SetTopLine(0);
	SetVerticalScrollPos();
	InvalidateStyleRedraw();
}

void Editor::ClearDocumentStyle() {
	pdoc->decorations->DeleteLexerDecorations();
	const Sci::Position endStyled = pdoc->GetEndStyled();
	pdoc->StartStyling(0);
	pdoc->SetStyleFor(endStyled, 0);
	pcs->ShowAll();
	SetAnnotationHeights(0, pdoc->LinesTotal());
	pdoc->ClearLevels();
}

void Editor::CopyAllowLine() {
	SelectionText selectedText;
	CopySelectionRange(&selectedText, true);
	CopyToClipboard(selectedText);
}

void Editor::Cut(bool asBinary, bool lineCopy) {
	pdoc->CheckReadOnly();
	if (!pdoc->IsReadOnly() && !SelectionContainsProtected()) {
		if (lineCopy) {
			CopyRangeToClipboard(sel.RangeMain().Start().Position(), sel.RangeMain().End().Position(), true);
		} else {
			Copy(asBinary);
		}
		ClearSelection();
	}
}

void Editor::PasteRectangular(SelectionPosition pos, const char *ptr, Sci::Position len) {
	if (pdoc->IsReadOnly() || SelectionContainsProtected()) {
		return;
	}
	sel.Clear();
	sel.RangeMain() = SelectionRange(pos);
	Sci::Line line = pdoc->SciLineFromPosition(sel.MainCaret());
	UndoGroup ug(pdoc);
	sel.RangeMain().caret = RealizeVirtualSpace(sel.RangeMain().caret);
	const int xInsert = XFromPosition(sel.RangeMain().caret);
	bool prevCr = false;
	while ((len > 0) && IsEOLChar(ptr[len - 1])) {
		len--;
	}
	for (Sci::Position i = 0; i < len; i++) {
		if (IsEOLChar(ptr[i])) {
			if ((ptr[i] == '\r') || (!prevCr))
				line++;
			if (line >= pdoc->LinesTotal()) {
				if (pdoc->eolMode != SC_EOL_LF)
					pdoc->InsertString(pdoc->Length(), "\r", 1);
				if (pdoc->eolMode != SC_EOL_CR)
					pdoc->InsertString(pdoc->Length(), "\n", 1);
			}
			// Pad the end of lines with spaces if required
			sel.RangeMain().caret.SetPosition(PositionFromLineX(line, xInsert));
			if ((XFromPosition(sel.RangeMain().caret) < xInsert) && (i + 1 < len)) {
				while (XFromPosition(sel.RangeMain().caret) < xInsert) {
					assert(pdoc);
					const Sci::Position lengthInserted = pdoc->InsertString(sel.MainCaret(), " ", 1);
					sel.RangeMain().caret.Add(lengthInserted);
				}
			}
			prevCr = ptr[i] == '\r';
		} else {
			const Sci::Position lengthInserted = pdoc->InsertString(sel.MainCaret(), ptr + i, 1);
			sel.RangeMain().caret.Add(lengthInserted);
			prevCr = false;
		}
	}
	SetEmptySelection(pos);
}

bool Editor::CanPaste() noexcept {
	return !pdoc->IsReadOnly() && !SelectionContainsProtected();
}

void Editor::Clear() {
	// If multiple selections, don't delete EOLS
	if (sel.Empty()) {
		bool singleVirtual = false;
		if ((sel.Count() == 1) &&
			!RangeContainsProtected(sel.MainCaret(), sel.MainCaret() + 1) &&
			sel.RangeMain().Start().VirtualSpace()) {
			singleVirtual = true;
		}
		UndoGroup ug(pdoc, (sel.Count() > 1) || singleVirtual);
		for (size_t r = 0; r < sel.Count(); r++) {
			const Sci::Position caretPosition = sel.Range(r).caret.Position();
			if (!RangeContainsProtected(caretPosition, caretPosition + 1)) {
				if (sel.Range(r).Start().VirtualSpace()) {
					if (sel.Range(r).anchor < sel.Range(r).caret)
						sel.Range(r) = SelectionRange(RealizeVirtualSpace(caretPosition, sel.Range(r).anchor.VirtualSpace()));
					else
						sel.Range(r) = SelectionRange(RealizeVirtualSpace(caretPosition, sel.Range(r).caret.VirtualSpace()));
				}
				if ((sel.Count() == 1) || !pdoc->IsPositionInLineEnd(caretPosition)) {
					pdoc->DelChar(caretPosition);
					sel.Range(r).ClearVirtualSpace();
				}  // else multiple selection so don't eat line ends
			} else {
				sel.Range(r).ClearVirtualSpace();
			}
		}
	} else {
		ClearSelection();
	}
	sel.RemoveDuplicates();
	ShowCaretAtCurrentPosition();		// Avoid blinking
}

void Editor::SelectAll() {
	sel.Clear();
	SetSelection(0, pdoc->Length());
	Redraw();
}

void Editor::Undo() {
	if (pdoc->CanUndo()) {
		InvalidateCaret();
		const Sci::Position newPos = pdoc->Undo();
		if (newPos >= 0)
			SetEmptySelection(newPos);
		EnsureCaretVisible();
	}
}

void Editor::Redo() {
	if (pdoc->CanRedo()) {
		const Sci::Position newPos = pdoc->Redo();
		if (newPos >= 0)
			SetEmptySelection(newPos);
		EnsureCaretVisible();
	}
}

bool Editor::BackspaceUnindent(Sci::Position lineCurrentPos, Sci::Position caretPosition, Sci::Position *posSelect) {
	const char chPrev = pdoc->CharAt(caretPosition - 1);
	if (!IsSpaceOrTab(chPrev)) {
		return false;
	}

	const Sci::Position column = pdoc->GetColumn(caretPosition);
	const int indentation = pdoc->GetLineIndentation(lineCurrentPos);
	if (column > 0 && (column <= indentation || chPrev == ' ')) {
		const int indentationStep = pdoc->IndentSize();
		Sci::Position indentationChange = column % indentationStep;
		if (indentationChange == 0) {
			indentationChange = indentationStep;
		}
		if (column <= indentation && pdoc->backspaceUnindents) {
			//UndoGroup ugInner(pdoc, !ug.Needed());
			*posSelect = pdoc->SetLineIndentation(lineCurrentPos, indentation - indentationChange);
			return true;
		}
		if (indentationChange > 1) {
			const Sci_Position minPos = std::max(lineCurrentPos, caretPosition - indentationChange);
			Sci::Position pos = caretPosition - 1;
			while (pos >= minPos && pdoc->CharAt(pos) == ' ') {
				--pos;
			}
			++pos;
			if (pos == minPos) {
				pdoc->DeleteChars(pos, indentationChange);
				*posSelect = pos;
				return true;
			}
		}
	}
	return false;
}

void Editor::DelCharBack(bool allowLineStartDeletion) {
	RefreshStyleData();
	if (!sel.IsRectangular())
		FilterSelections();
	if (sel.IsRectangular())
		allowLineStartDeletion = false;
	UndoGroup ug(pdoc, (sel.Count() > 1) || !sel.Empty());
	if (sel.Empty()) {
		for (size_t r = 0; r < sel.Count(); r++) {
			const Sci::Position caretPosition = sel.Range(r).caret.Position();
			if (!RangeContainsProtected(caretPosition - 1, caretPosition)) {
				if (sel.Range(r).caret.VirtualSpace()) {
					sel.Range(r).caret.SetVirtualSpace(sel.Range(r).caret.VirtualSpace() - 1);
					sel.Range(r).anchor.SetVirtualSpace(sel.Range(r).caret.VirtualSpace());
				} else {
					const Sci::Line lineCurrentPos = pdoc->SciLineFromPosition(caretPosition);
					if (allowLineStartDeletion || (pdoc->LineStart(lineCurrentPos) != caretPosition)) {
						Sci::Position posSelect;
						if (BackspaceUnindent(lineCurrentPos, caretPosition, &posSelect)) {
							// SetEmptySelection
							sel.Range(r) = SelectionRange(posSelect);
						} else {
							pdoc->DelCharBack(caretPosition);
						}
					}
				}
			} else {
				sel.Range(r).ClearVirtualSpace();
			}
		}
		ThinRectangularRange();
	} else {
		ClearSelection();
	}
	sel.RemoveDuplicates();
	ContainerNeedsUpdate(SC_UPDATE_SELECTION);
	// Avoid blinking during rapid typing:
	ShowCaretAtCurrentPosition();
}

void Editor::NotifyFocus(bool focus) {
	SCNotification scn = {};
	scn.nmhdr.code = focus ? SCN_FOCUSIN : SCN_FOCUSOUT;
	NotifyParent(scn);
}

void Editor::SetCtrlID(int identifier) noexcept {
	ctrlID = identifier;
}

void Editor::NotifyStyleToNeeded(Sci::Position endStyleNeeded) {
	SCNotification scn = {};
	scn.nmhdr.code = SCN_STYLENEEDED;
	scn.position = endStyleNeeded;
	NotifyParent(scn);
}

void Editor::NotifyStyleNeeded(Document *, void *, Sci::Position endStyleNeeded) {
	NotifyStyleToNeeded(endStyleNeeded);
}

void Editor::NotifyLexerChanged(Document *, void *) {
}

void Editor::NotifyErrorOccurred(Document *, void *, int status) noexcept {
	errorStatus = status;
}

void Editor::NotifyChar(int ch, CharacterSource charSource) noexcept {
	SCNotification scn = {};
	scn.nmhdr.code = SCN_CHARADDED;
	scn.ch = ch;
	scn.characterSource = static_cast<int>(charSource);
	NotifyParent(scn);
}

void Editor::NotifySavePoint(bool isSavePoint) noexcept {
	SCNotification scn = {};
	if (isSavePoint) {
		scn.nmhdr.code = SCN_SAVEPOINTREACHED;
	} else {
		scn.nmhdr.code = SCN_SAVEPOINTLEFT;
	}
	NotifyParent(scn);
}

void Editor::NotifyModifyAttempt() noexcept {
	SCNotification scn = {};
	scn.nmhdr.code = SCN_MODIFYATTEMPTRO;
	NotifyParent(scn);
}

void Editor::NotifyDoubleClick(Point pt, int modifiers) {
	SCNotification scn = {};
	scn.nmhdr.code = SCN_DOUBLECLICK;
	scn.line = LineFromLocation(pt);
	scn.position = PositionFromLocation(pt, true);
	scn.modifiers = modifiers;
	NotifyParent(scn);
}

void Editor::NotifyHotSpotDoubleClicked(Sci::Position position, int modifiers) noexcept {
	SCNotification scn = {};
	scn.nmhdr.code = SCN_HOTSPOTDOUBLECLICK;
	scn.position = position;
	scn.modifiers = modifiers;
	NotifyParent(scn);
}

void Editor::NotifyHotSpotClicked(Sci::Position position, int modifiers) noexcept {
	SCNotification scn = {};
	scn.nmhdr.code = SCN_HOTSPOTCLICK;
	scn.position = position;
	scn.modifiers = modifiers;
	NotifyParent(scn);
}

void Editor::NotifyHotSpotReleaseClick(Sci::Position position, int modifiers) noexcept {
	SCNotification scn = {};
	scn.nmhdr.code = SCN_HOTSPOTRELEASECLICK;
	scn.position = position;
	scn.modifiers = modifiers;
	NotifyParent(scn);
}

bool Editor::NotifyUpdateUI() noexcept {
	if (needUpdateUI) {
		SCNotification scn = {};
		scn.nmhdr.code = SCN_UPDATEUI;
		scn.updated = needUpdateUI;
		NotifyParent(scn);
		needUpdateUI = 0;
		return true;
	}
	return false;
}

void Editor::NotifyPainted() noexcept {
#if 0 // avoid very frequent notification
	SCNotification scn = {};
	scn.nmhdr.code = SCN_PAINTED;
	NotifyParent(scn);
#endif
}

void Editor::NotifyIndicatorClick(bool click, Sci::Position position, int modifiers) noexcept {
	const int mask = pdoc->decorations->AllOnFor(position);
	if ((click && mask) || pdoc->decorations->ClickNotified()) {
		SCNotification scn = {};
		pdoc->decorations->SetClickNotified(click);
		scn.nmhdr.code = click ? SCN_INDICATORCLICK : SCN_INDICATORRELEASE;
		scn.modifiers = modifiers;
		scn.position = position;
		NotifyParent(scn);
	}
}

bool Editor::NotifyMarginClick(Point pt, int modifiers) {
	const int marginClicked = vs.MarginFromLocation(pt);
	if ((marginClicked >= 0) && vs.ms[marginClicked].sensitive) {
		const Sci::Position position = pdoc->LineStart(LineFromLocation(pt));
		if ((vs.ms[marginClicked].mask & SC_MASK_FOLDERS) && (foldAutomatic & SC_AUTOMATICFOLD_CLICK)) {
			const bool ctrl = (modifiers & SCI_CTRL) != 0;
			const bool shift = (modifiers & SCI_SHIFT) != 0;
			const Sci::Line lineClick = pdoc->SciLineFromPosition(position);
			if (shift && ctrl) {
				FoldAll(SC_FOLDACTION_TOGGLE);
			} else {
				const int levelClick = pdoc->GetLevel(lineClick);
				if (levelClick & SC_FOLDLEVELHEADERFLAG) {
					if (shift) {
						// Ensure all children visible
						FoldExpand(lineClick, SC_FOLDACTION_EXPAND, levelClick);
					} else if (ctrl) {
						FoldExpand(lineClick, SC_FOLDACTION_TOGGLE, levelClick);
					} else {
						// Toggle this line
						FoldLine(lineClick, SC_FOLDACTION_TOGGLE);
					}
				}
			}
			return true;
		}
		SCNotification scn = {};
		scn.nmhdr.code = SCN_MARGINCLICK;
		scn.modifiers = modifiers;
		scn.position = position;
		scn.margin = marginClicked;
		NotifyParent(scn);
		return true;
	} else {
		return false;
	}
}

bool Editor::NotifyMarginRightClick(Point pt, int modifiers) {
	const int marginRightClicked = vs.MarginFromLocation(pt);
	if ((marginRightClicked >= 0) && vs.ms[marginRightClicked].sensitive) {
		const Sci::Position position = pdoc->LineStart(LineFromLocation(pt));
		SCNotification scn = {};
		scn.nmhdr.code = SCN_MARGINRIGHTCLICK;
		scn.modifiers = modifiers;
		scn.position = position;
		scn.margin = marginRightClicked;
		NotifyParent(scn);
		return true;
	} else {
		return false;
	}
}

void Editor::NotifyNeedShown(Sci::Position pos, Sci::Position len) noexcept {
	SCNotification scn = {};
	scn.nmhdr.code = SCN_NEEDSHOWN;
	scn.position = pos;
	scn.length = len;
	NotifyParent(scn);
}

void Editor::NotifyCodePageChanged(int oldCodePage) noexcept {
	SCNotification scn = {};
	scn.nmhdr.code = SCN_CODEPAGECHANGED;
	scn.oldCodePage = oldCodePage;
	NotifyParent(scn);
}

void Editor::NotifyDwelling(Point pt, bool state) {
	SCNotification scn = {};
	scn.nmhdr.code = state ? SCN_DWELLSTART : SCN_DWELLEND;
	scn.position = PositionFromLocation(pt, true);
	scn.x = static_cast<int>(pt.x + vs.ExternalMarginWidth());
	scn.y = static_cast<int>(pt.y);
	NotifyParent(scn);
}

void Editor::NotifyZoom() noexcept {
	SCNotification scn = {};
	scn.nmhdr.code = SCN_ZOOM;
	NotifyParent(scn);
}

// Notifications from document
void Editor::NotifyModifyAttempt(Document *, void *) noexcept {
	//Platform::DebugPrintf("** Modify Attempt\n");
	NotifyModifyAttempt();
}

void Editor::NotifySavePoint(Document *, void *, bool atSavePoint) noexcept {
	//Platform::DebugPrintf("** Save Point %s\n", atSavePoint ? "On" : "Off");
	NotifySavePoint(atSavePoint);
}

void Editor::CheckModificationForWrap(DocModification mh) {
	if (mh.modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) {
		view.llc.Invalidate(LineLayout::ValidLevel::checkTextAndStyle);
		const Sci::Line lineDoc = pdoc->SciLineFromPosition(mh.position);
		const Sci::Line lines = std::max<Sci::Line>(0, mh.linesAdded);
		if (Wrapping()) {
			NeedWrapping(lineDoc, lineDoc + lines + 1);
		}
		RefreshStyleData();
		// Fix up annotation heights
		SetAnnotationHeights(lineDoc, lineDoc + lines + 2);
	}
}

namespace {

// Move a position so it is still after the same character as before the insertion.
constexpr Sci::Position MovePositionForInsertion(Sci::Position position, Sci::Position startInsertion, Sci::Position length) noexcept {
	if (position > startInsertion) {
		return position + length;
	}
	return position;
}

// Move a position so it is still after the same character as before the deletion if that
// character is still present else after the previous surviving character.
constexpr Sci::Position MovePositionForDeletion(Sci::Position position, Sci::Position startDeletion, Sci::Position length) noexcept {
	if (position > startDeletion) {
		const Sci::Position endDeletion = startDeletion + length;
		if (position > endDeletion) {
			return position - length;
		} else {
			return startDeletion;
		}
	} else {
		return position;
	}
}

}

void Editor::NotifyModified(Document *, DocModification mh, void *) {
	if (mh.modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) {
		ContainerNeedsUpdate(SC_UPDATE_CONTENT);
	}
	if (paintState == PaintState::painting) {
		CheckForChangeOutsidePaint(Range(mh.position, mh.position + mh.length));
	}
	if (mh.modificationType & SC_MOD_CHANGELINESTATE) {
		if (paintState == PaintState::painting) {
			CheckForChangeOutsidePaint(
				Range(pdoc->LineStart(mh.line),
					pdoc->LineStart(mh.line + 1)));
		} else {
			// Could check that change is before last visible line.
			Redraw();
		}
	}
	if (mh.modificationType & SC_MOD_CHANGETABSTOPS) {
		Redraw();
	}
	if (mh.modificationType & SC_MOD_LEXERSTATE) {
		if (paintState == PaintState::painting) {
			CheckForChangeOutsidePaint(
				Range(mh.position, mh.position + mh.length));
		} else {
			Redraw();
		}
	}
	if (mh.modificationType & (SC_MOD_CHANGESTYLE | SC_MOD_CHANGEINDICATOR)) {
		if (mh.modificationType & SC_MOD_CHANGESTYLE) {
			pdoc->IncrementStyleClock();
		}
		if (paintState == PaintState::notPainting) {
			const Sci::Line lineDocTop = pcs->DocFromDisplay(topLine);
			if (mh.position < pdoc->LineStart(lineDocTop)) {
				// Styling performed before this view
				Redraw();
			} else {
				InvalidateRange(mh.position, mh.position + mh.length);
			}
		}
		if (mh.modificationType & SC_MOD_CHANGESTYLE) {
			view.llc.Invalidate(LineLayout::ValidLevel::checkTextAndStyle);
		}
	} else {
		// Move selection and brace highlights
		if (mh.modificationType & SC_MOD_INSERTTEXT) {
			sel.MovePositions(true, mh.position, mh.length);
			braces[0] = MovePositionForInsertion(braces[0], mh.position, mh.length);
			braces[1] = MovePositionForInsertion(braces[1], mh.position, mh.length);
		} else if (mh.modificationType & SC_MOD_DELETETEXT) {
			sel.MovePositions(false, mh.position, mh.length);
			braces[0] = MovePositionForDeletion(braces[0], mh.position, mh.length);
			braces[1] = MovePositionForDeletion(braces[1], mh.position, mh.length);
		}
		if ((mh.modificationType & (SC_MOD_BEFOREINSERT | SC_MOD_BEFOREDELETE)) && pcs->HiddenLines()) {
			// Some lines are hidden so may need shown.
			const Sci::Line lineOfPos = pdoc->SciLineFromPosition(mh.position);
			Sci::Position endNeedShown = mh.position;
			if (mh.modificationType & SC_MOD_BEFOREINSERT) {
				if (pdoc->ContainsLineEnd(mh.text, mh.length) && (mh.position != pdoc->LineStart(lineOfPos)))
					endNeedShown = pdoc->LineStart(lineOfPos + 1);
			} else if (mh.modificationType & SC_MOD_BEFOREDELETE) {
				// If the deletion includes any EOL then we extend the need shown area.
				endNeedShown = mh.position + mh.length;
				Sci::Line lineLast = pdoc->SciLineFromPosition(mh.position + mh.length);
				for (Sci::Line line = lineOfPos + 1; line <= lineLast; line++) {
					const Sci::Line lineMaxSubord = pdoc->GetLastChild(line, -1, -1);
					if (lineLast < lineMaxSubord) {
						lineLast = lineMaxSubord;
						endNeedShown = pdoc->LineEnd(lineLast);
					}
				}
			}
			NeedShown(mh.position, endNeedShown - mh.position);
		}
		if (mh.linesAdded != 0) {
			// Update contraction state for inserted and removed lines
			// lineOfPos should be calculated in context of state before modification, shouldn't it
			Sci::Line lineOfPos = pdoc->SciLineFromPosition(mh.position);
			if (mh.position > pdoc->LineStart(lineOfPos))
				lineOfPos++;	// Affecting subsequent lines
			if (mh.linesAdded > 0) {
				pcs->InsertLines(lineOfPos, mh.linesAdded);
			} else {
				pcs->DeleteLines(lineOfPos, -mh.linesAdded);
			}
			view.LinesAddedOrRemoved(lineOfPos, mh.linesAdded);
		}
		if (mh.modificationType & SC_MOD_CHANGEANNOTATION) {
			const Sci::Line lineDoc = pdoc->SciLineFromPosition(mh.position);
			if (vs.annotationVisible) {
				if (pcs->SetHeight(lineDoc, pcs->GetHeight(lineDoc) + static_cast<int>(mh.annotationLinesAdded))) {
					SetScrollBars();
				}
				Redraw();
			}
		}
		if (mh.modificationType & SC_MOD_CHANGEEOLANNOTATION) {
			if (vs.eolAnnotationVisible) {
				Redraw();
			}
		}
		CheckModificationForWrap(mh);
		if (mh.linesAdded != 0) {
			// Avoid scrolling of display if change before current display
			if (mh.position < posTopLine && !CanDeferToLastStep(mh)) {
				const Sci::Line newTop = std::clamp<Sci::Line>(topLine + mh.linesAdded, 0, MaxScrollPos());
				if (newTop != topLine) {
					SetTopLine(newTop);
					SetVerticalScrollPos();
				}
			}

			if (paintState == PaintState::notPainting && !CanDeferToLastStep(mh)) {
				if (SynchronousStylingToVisible()) {
					QueueIdleWork(WorkItems::style, pdoc->Length());
				}
				Redraw();
			}
		} else {
			if (paintState == PaintState::notPainting && mh.length && !CanEliminate(mh)) {
				if (SynchronousStylingToVisible()) {
					QueueIdleWork(WorkItems::style, mh.position + mh.length);
				}
				InvalidateRange(mh.position, mh.position + mh.length);
			}
		}
	}

	if (mh.linesAdded != 0 && !CanDeferToLastStep(mh)) {
		SetScrollBars();
	}

	if ((mh.modificationType & SC_MOD_CHANGEMARKER) || (mh.modificationType & SC_MOD_CHANGEMARGIN)) {
		if ((!willRedrawAll) && ((paintState == PaintState::notPainting) || !PaintContainsMargin())) {
			if (mh.modificationType & SC_MOD_CHANGEFOLD) {
				// Fold changes can affect the drawing of following lines so redraw whole margin
				RedrawSelMargin(marginView.highlightDelimiter.isEnabled ? -1 : mh.line - 1, true);
			} else {
				RedrawSelMargin(mh.line);
			}
		}
	}
	if ((mh.modificationType & SC_MOD_CHANGEFOLD) && (foldAutomatic & SC_AUTOMATICFOLD_CHANGE)) {
		FoldChanged(mh.line, mh.foldLevelNow, mh.foldLevelPrev);
	}

	// NOW pay the piper WRT "deferred" visual updates
	if (IsLastStep(mh)) {
		SetScrollBars();
		Redraw();
	}

	// If client wants to see this modification
	if (mh.modificationType & modEventMask) {
		if (commandEvents) {
			if ((mh.modificationType & (SC_MOD_CHANGESTYLE | SC_MOD_CHANGEINDICATOR)) == 0) {
				// Real modification made to text of document.
				NotifyChange();	// Send EN_CHANGE
			}
		}

		SCNotification scn = {};
		scn.nmhdr.code = SCN_MODIFIED;
		scn.position = mh.position;
		scn.modificationType = mh.modificationType;
		scn.text = mh.text;
		scn.length = mh.length;
		scn.linesAdded = mh.linesAdded;
		scn.line = mh.line;
		scn.foldLevelNow = mh.foldLevelNow;
		scn.foldLevelPrev = mh.foldLevelPrev;
		scn.token = static_cast<int>(mh.token);
		scn.annotationLinesAdded = mh.annotationLinesAdded;
		NotifyParent(scn);
	}
}

void Editor::NotifyDeleted(Document *, void *) noexcept {
	/* Do nothing */
}

void Editor::NotifyMacroRecord(unsigned int iMessage, uptr_t wParam, sptr_t lParam) noexcept {

	// Enumerates all macroable messages
	switch (iMessage) {
	case SCI_CUT:
	case SCI_COPY:
	case SCI_PASTE:
	case SCI_CLEAR:
	case SCI_REPLACESEL:
	case SCI_ADDTEXT:
	case SCI_INSERTTEXT:
	case SCI_APPENDTEXT:
	case SCI_CLEARALL:
	case SCI_SELECTALL:
	case SCI_GOTOLINE:
	case SCI_GOTOPOS:
	case SCI_SEARCHANCHOR:
	case SCI_SEARCHNEXT:
	case SCI_SEARCHPREV:
	case SCI_LINEDOWN:
	case SCI_LINEDOWNEXTEND:
	case SCI_PARADOWN:
	case SCI_PARADOWNEXTEND:
	case SCI_LINEUP:
	case SCI_LINEUPEXTEND:
	case SCI_PARAUP:
	case SCI_PARAUPEXTEND:
	case SCI_CHARLEFT:
	case SCI_CHARLEFTEXTEND:
	case SCI_CHARRIGHT:
	case SCI_CHARRIGHTEXTEND:
	case SCI_WORDLEFT:
	case SCI_WORDLEFTEXTEND:
	case SCI_WORDRIGHT:
	case SCI_WORDRIGHTEXTEND:
	case SCI_WORDPARTLEFT:
	case SCI_WORDPARTLEFTEXTEND:
	case SCI_WORDPARTRIGHT:
	case SCI_WORDPARTRIGHTEXTEND:
	case SCI_WORDLEFTEND:
	case SCI_WORDLEFTENDEXTEND:
	case SCI_WORDRIGHTEND:
	case SCI_WORDRIGHTENDEXTEND:
	case SCI_HOME:
	case SCI_HOMEEXTEND:
	case SCI_LINEEND:
	case SCI_LINEENDEXTEND:
	case SCI_HOMEWRAP:
	case SCI_HOMEWRAPEXTEND:
	case SCI_LINEENDWRAP:
	case SCI_LINEENDWRAPEXTEND:
	case SCI_DOCUMENTSTART:
	case SCI_DOCUMENTSTARTEXTEND:
	case SCI_DOCUMENTEND:
	case SCI_DOCUMENTENDEXTEND:
	case SCI_STUTTEREDPAGEUP:
	case SCI_STUTTEREDPAGEUPEXTEND:
	case SCI_STUTTEREDPAGEDOWN:
	case SCI_STUTTEREDPAGEDOWNEXTEND:
	case SCI_PAGEUP:
	case SCI_PAGEUPEXTEND:
	case SCI_PAGEDOWN:
	case SCI_PAGEDOWNEXTEND:
	case SCI_EDITTOGGLEOVERTYPE:
	case SCI_CANCEL:
	case SCI_DELETEBACK:
	case SCI_TAB:
	case SCI_BACKTAB:
	case SCI_FORMFEED:
	case SCI_VCHOME:
	case SCI_VCHOMEEXTEND:
	case SCI_VCHOMEWRAP:
	case SCI_VCHOMEWRAPEXTEND:
	case SCI_VCHOMEDISPLAY:
	case SCI_VCHOMEDISPLAYEXTEND:
	case SCI_DELWORDLEFT:
	case SCI_DELWORDRIGHT:
	case SCI_DELWORDRIGHTEND:
	case SCI_DELLINELEFT:
	case SCI_DELLINERIGHT:
	case SCI_LINECOPY:
	case SCI_LINECUT:
	case SCI_LINEDELETE:
	case SCI_LINETRANSPOSE:
	case SCI_LINEREVERSE:
	case SCI_LINEDUPLICATE:
	case SCI_LOWERCASE:
	case SCI_UPPERCASE:
	case SCI_LINESCROLLDOWN:
	case SCI_LINESCROLLUP:
	case SCI_DELETEBACKNOTLINE:
	case SCI_HOMEDISPLAY:
	case SCI_HOMEDISPLAYEXTEND:
	case SCI_LINEENDDISPLAY:
	case SCI_LINEENDDISPLAYEXTEND:
	case SCI_SETSELECTIONMODE:
	case SCI_LINEDOWNRECTEXTEND:
	case SCI_LINEUPRECTEXTEND:
	case SCI_CHARLEFTRECTEXTEND:
	case SCI_CHARRIGHTRECTEXTEND:
	case SCI_HOMERECTEXTEND:
	case SCI_VCHOMERECTEXTEND:
	case SCI_LINEENDRECTEXTEND:
	case SCI_PAGEUPRECTEXTEND:
	case SCI_PAGEDOWNRECTEXTEND:
	case SCI_SELECTIONDUPLICATE:
	case SCI_COPYALLOWLINE:
	case SCI_VERTICALCENTRECARET:
	case SCI_MOVESELECTEDLINESUP:
	case SCI_MOVESELECTEDLINESDOWN:
	case SCI_SCROLLTOSTART:
	case SCI_SCROLLTOEND:
		break;

		// Filter out all others like display changes. Also, newlines are redundant
		// with char insert messages.
	case SCI_NEWLINE:
	default:
		//Platform::DebugPrintf("Filtered out %u of macro recording\n", iMessage);
		return;
	}

	// Send notification
	SCNotification scn = {};
	scn.nmhdr.code = SCN_MACRORECORD;
	scn.message = iMessage;
	scn.wParam = wParam;
	scn.lParam = lParam;
	NotifyParent(scn);
}

// Something has changed that the container should know about
void Editor::ContainerNeedsUpdate(int flags) noexcept {
	needUpdateUI |= flags;
}

/**
 * Force scroll and keep position relative to top of window.
 *
 * If stuttered = true and not already at first/last row, move to first/last row of window.
 * If stuttered = true and already at first/last row, scroll as normal.
 */
void Editor::PageMove(int direction, Selection::SelTypes selt, bool stuttered) {
	Sci::Line topLineNew;
	SelectionPosition newPos;

	const Sci::Line currentLine = pdoc->SciLineFromPosition(sel.MainCaret());
	const Sci::Line topStutterLine = topLine + caretPolicies.y.slop;
	const Sci::Line bottomStutterLine =
		pdoc->SciLineFromPosition(PositionFromLocation(
			Point::FromInts(lastXChosen - xOffset, direction * vs.lineHeight * static_cast<int>(LinesToScroll()))))
		- caretPolicies.y.slop - 1;

	if (stuttered && (direction < 0 && currentLine > topStutterLine)) {
		topLineNew = topLine;
		newPos = SPositionFromLocation(Point::FromInts(lastXChosen - xOffset, vs.lineHeight * caretPolicies.y.slop),
			false, false, UserVirtualSpace());

	} else if (stuttered && (direction > 0 && currentLine < bottomStutterLine)) {
		topLineNew = topLine;
		newPos = SPositionFromLocation(Point::FromInts(lastXChosen - xOffset, vs.lineHeight * static_cast<int>(LinesToScroll() - caretPolicies.y.slop)),
			false, false, UserVirtualSpace());

	} else {
		const Point pt = LocationFromPosition(sel.MainCaret());

		topLineNew = std::clamp<Sci::Line>(
			topLine + direction * LinesToScroll(), 0, MaxScrollPos());
		newPos = SPositionFromLocation(
			Point::FromInts(lastXChosen - xOffset, static_cast<int>(pt.y) +
				direction * (vs.lineHeight * static_cast<int>(LinesToScroll()))),
			false, false, UserVirtualSpace());
	}

	if (topLineNew != topLine) {
		SetTopLine(topLineNew);
		MovePositionTo(newPos, selt);
		Redraw();
		SetVerticalScrollPos();
	} else {
		MovePositionTo(newPos, selt);
	}
}

void Editor::ChangeCaseOfSelection(CaseMapping caseMapping) {
	UndoGroup ug(pdoc);
	for (size_t r = 0; r < sel.Count(); r++) {
		SelectionRange current = sel.Range(r);
		SelectionRange currentNoVS = current;
		currentNoVS.ClearVirtualSpace();
		const size_t rangeBytes = currentNoVS.Length();
		if (rangeBytes > 0) {
			const std::string sText = RangeText(currentNoVS.Start().Position(), currentNoVS.End().Position());

			const std::string sMapped = CaseMapString(sText, caseMapping);

			if (sMapped != sText) {
				size_t firstDifference = 0;
				while (sMapped[firstDifference] == sText[firstDifference]) {
					firstDifference++;
				}
				size_t lastDifferenceText = sText.size() - 1;
				size_t lastDifferenceMapped = sMapped.size() - 1;
				while (sMapped[lastDifferenceMapped] == sText[lastDifferenceText]) {
					lastDifferenceText--;
					lastDifferenceMapped--;
				}
				const size_t endDifferenceText = sText.size() - 1 - lastDifferenceText;
				pdoc->DeleteChars(
					currentNoVS.Start().Position() + firstDifference,
					rangeBytes - firstDifference - endDifferenceText);
				const Sci::Position lengthChange = lastDifferenceMapped - firstDifference + 1;
				const Sci::Position lengthInserted = pdoc->InsertString(
					currentNoVS.Start().Position() + firstDifference,
					sMapped.c_str() + firstDifference,
					lengthChange);
				// Automatic movement changes selection so reset to exactly the same as it was.
				const Sci::Position diffSizes = sMapped.size() - sText.size() + lengthInserted - lengthChange;
				if (diffSizes != 0) {
					if (current.anchor > current.caret)
						current.anchor.Add(diffSizes);
					else
						current.caret.Add(diffSizes);
				}
				sel.Range(r) = current;
			}
		}
	}
}

void Editor::LineTranspose() {
	const Sci::Line line = pdoc->SciLineFromPosition(sel.MainCaret());
	if (line > 0) {
		UndoGroup ug(pdoc);

		const Sci::Position startPrevious = pdoc->LineStart(line - 1);
		const std::string linePrevious = RangeText(startPrevious, pdoc->LineEnd(line - 1));

		Sci::Position startCurrent = pdoc->LineStart(line);
		const std::string lineCurrent = RangeText(startCurrent, pdoc->LineEnd(line));

		pdoc->DeleteChars(startCurrent, lineCurrent.length());
		pdoc->DeleteChars(startPrevious, linePrevious.length());
		startCurrent -= linePrevious.length();

		startCurrent += pdoc->InsertString(startPrevious, lineCurrent.c_str(),
			lineCurrent.length());
		pdoc->InsertString(startCurrent, linePrevious.c_str(),
			linePrevious.length());
		// Move caret to start of current line
		MovePositionTo(SelectionPosition(startCurrent));
	}
}

void Editor::LineReverse() {
	const Sci::Line lineStart =
		pdoc->SciLineFromPosition(sel.RangeMain().Start().Position());
	const Sci::Line lineEnd =
		pdoc->SciLineFromPosition(sel.RangeMain().End().Position() - 1);
	const Sci::Line lineDiff = lineEnd - lineStart;
	if (lineDiff <= 0)
		return;
	UndoGroup ug(pdoc);
	for (Sci::Line i = (lineDiff + 1) / 2 - 1; i >= 0; --i) {
		const Sci::Line lineNum2 = lineEnd - i;
		const Sci::Line lineNum1 = lineStart + i;
		Sci::Position lineStart2 = pdoc->LineStart(lineNum2);
		const Sci::Position lineStart1 = pdoc->LineStart(lineNum1);
		const std::string line2 = RangeText(lineStart2, pdoc->LineEnd(lineNum2));
		const std::string line1 = RangeText(lineStart1, pdoc->LineEnd(lineNum1));
		const Sci::Position lineLen2 = line2.length();
		const Sci::Position lineLen1 = line1.length();
		pdoc->DeleteChars(lineStart2, lineLen2);
		pdoc->DeleteChars(lineStart1, lineLen1);
		lineStart2 -= lineLen1;
		pdoc->InsertString(lineStart2, line1.c_str(), lineLen1);
		pdoc->InsertString(lineStart1, line2.c_str(), lineLen2);
	}
	// Wholly select all affected lines
	sel.RangeMain() = SelectionRange(pdoc->LineStart(lineStart),
		pdoc->LineStart(lineEnd + 1));
}

void Editor::Duplicate(bool forLine) {
	if (sel.Empty()) {
		forLine = true;
	}
	UndoGroup ug(pdoc);
	const char *eol = "";
	Sci::Position eolLen = 0;
	if (forLine) {
		eol = StringFromEOLMode(pdoc->eolMode);
		eolLen = strlen(eol);
	}
	for (size_t r = 0; r < sel.Count(); r++) {
		SelectionPosition start = sel.Range(r).Start();
		SelectionPosition end = sel.Range(r).End();
		if (forLine) {
			const Sci::Line line = pdoc->SciLineFromPosition(sel.Range(r).caret.Position());
			start = SelectionPosition(pdoc->LineStart(line));
			end = SelectionPosition(pdoc->LineEnd(line));
		}
		std::string text = RangeText(start.Position(), end.Position());
		Sci::Position lengthInserted = eolLen;
		if (forLine)
			lengthInserted = pdoc->InsertString(end.Position(), eol, eolLen);
		pdoc->InsertString(end.Position() + lengthInserted, text.c_str(), text.length());
	}
	if (sel.Count() && sel.IsRectangular()) {
		SelectionPosition last = sel.Last();
		if (forLine) {
			const Sci::Line line = pdoc->SciLineFromPosition(last.Position());
			last = SelectionPosition(last.Position() +
				pdoc->LineStart(line + 1) - pdoc->LineStart(line));
		}
		if (sel.Rectangular().anchor > sel.Rectangular().caret)
			sel.Rectangular().anchor = last;
		else
			sel.Rectangular().caret = last;
		SetRectangularRange();
	}
}

void Editor::CancelModes() noexcept {
	sel.SetMoveExtends(false);
}

void Editor::NewLine() {
	InvalidateWholeSelection();
	if (sel.IsRectangular() || !additionalSelectionTyping) {
		// Remove non-main ranges
		sel.DropAdditionalRanges();
	}

	UndoGroup ug(pdoc, !sel.Empty() || (sel.Count() > 1));

	// Clear each range
	if (!sel.Empty()) {
		ClearSelection();
	}

	// Insert each line end
	size_t countInsertions = 0;
	for (size_t r = 0; r < sel.Count(); r++) {
		sel.Range(r).ClearVirtualSpace();
		const char *eol = StringFromEOLMode(pdoc->eolMode);
		const Sci::Position positionInsert = sel.Range(r).caret.Position();
		const Sci::Line lineDoc = pdoc->SciLineFromPosition(positionInsert);
		const bool expanded = pcs->GetExpanded(lineDoc);
		const Sci::Position insertLength = pdoc->InsertString(positionInsert, eol, strlen(eol));
		if (insertLength > 0) {
			sel.Range(r) = SelectionRange(positionInsert + insertLength);
			countInsertions++;
			if (!expanded) {
				pcs->SetExpanded(lineDoc, true);
				pcs->SetExpanded(lineDoc + 1, false);
			}
		}
	}

	// Perform notifications after all the changes as the application may change the
	// selections in response to the characters.
	for (size_t i = 0; i < countInsertions; i++) {
		const char *eol = StringFromEOLMode(pdoc->eolMode);
		while (*eol) {
			NotifyChar(*eol, CharacterSource::directInput);
			if (recordingMacro) {
				char txt[2];
				txt[0] = *eol;
				txt[1] = '\0';
				NotifyMacroRecord(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>(txt));
			}
			eol++;
		}
	}

	SetLastXChosen();
	SetScrollBars();
	EnsureCaretVisible();
	// Avoid blinking during rapid typing:
	ShowCaretAtCurrentPosition();
}

SelectionPosition Editor::PositionUpOrDown(SelectionPosition spStart, int direction, int lastX) {
	const Point pt = LocationFromPosition(spStart);
	int skipLines = 0;

	if (vs.annotationVisible) {
		const Sci::Line lineDoc = pdoc->SciLineFromPosition(spStart.Position());
		const Point ptStartLine = LocationFromPosition(pdoc->LineStart(lineDoc));
		const int subLine = static_cast<int>(pt.y - ptStartLine.y) / vs.lineHeight;

		if (direction < 0 && subLine == 0) {
			const Sci::Line lineDisplay = pcs->DisplayFromDoc(lineDoc);
			if (lineDisplay > 0) {
				skipLines = pdoc->AnnotationLines(pcs->DocFromDisplay(lineDisplay - 1));
			}
		} else if (direction > 0 && subLine >= (pcs->GetHeight(lineDoc) - 1 - pdoc->AnnotationLines(lineDoc))) {
			skipLines = pdoc->AnnotationLines(lineDoc);
		}
	}

	const Sci::Line newY = static_cast<Sci::Line>(pt.y) + (1 + skipLines) * direction * vs.lineHeight;
	if (lastX < 0) {
		lastX = static_cast<int>(pt.x) + xOffset;
	}
	SelectionPosition posNew = SPositionFromLocation(
		Point::FromInts(lastX - xOffset, static_cast<int>(newY)), false, false, UserVirtualSpace());

	if (direction < 0) {
		// Line wrapping may lead to a location on the same line, so
		// seek back if that is the case.
		Point ptNew = LocationFromPosition(posNew.Position());
		while ((posNew.Position() > 0) && (pt.y == ptNew.y)) {
			posNew.Add(-1);
			posNew.SetVirtualSpace(0);
			ptNew = LocationFromPosition(posNew.Position());
		}
	} else if (direction > 0 && posNew.Position() != pdoc->Length()) {
		// There is an equivalent case when moving down which skips
		// over a line.
		Point ptNew = LocationFromPosition(posNew.Position());
		while ((posNew.Position() > spStart.Position()) && (ptNew.y > newY)) {
			posNew.Add(-1);
			posNew.SetVirtualSpace(0);
			ptNew = LocationFromPosition(posNew.Position());
		}
	}
	return posNew;
}

void Editor::CursorUpOrDown(int direction, Selection::SelTypes selt) {
	if ((selt == Selection::SelTypes::none) && sel.MoveExtends()) {
		selt = !sel.IsRectangular() ? Selection::SelTypes::stream : Selection::SelTypes::rectangle;
	}
	SelectionPosition caretToUse = sel.Range(sel.Main()).caret;
	if (sel.IsRectangular()) {
		if (selt == Selection::SelTypes::none) {
			caretToUse = (direction > 0) ? sel.Limits().end : sel.Limits().start;
		} else {
			caretToUse = sel.Rectangular().caret;
		}
	}
	if (selt == Selection::SelTypes::rectangle) {
		const SelectionRange rangeBase = sel.IsRectangular() ? sel.Rectangular() : sel.RangeMain();
		if (!sel.IsRectangular()) {
			InvalidateWholeSelection();
			sel.DropAdditionalRanges();
		}
		const SelectionPosition posNew = MovePositionSoVisible(
			PositionUpOrDown(caretToUse, direction, lastXChosen), direction);
		sel.selType = Selection::SelTypes::rectangle;
		sel.Rectangular() = SelectionRange(posNew, rangeBase.anchor);
		SetRectangularRange();
		MovedCaret(posNew, caretToUse, true, caretPolicies);
	} else if (sel.selType == Selection::SelTypes::lines && sel.MoveExtends()) {
		// Calculate new caret position and call SetSelection(), which will ensure whole lines are selected.
		const SelectionPosition posNew = MovePositionSoVisible(
			PositionUpOrDown(caretToUse, direction, -1), direction);
		SetSelection(posNew, sel.Range(sel.Main()).anchor);
	} else {
		InvalidateWholeSelection();
		if (!additionalSelectionTyping || (sel.IsRectangular())) {
			sel.DropAdditionalRanges();
		}
		sel.selType = Selection::SelTypes::stream;
		for (size_t r = 0; r < sel.Count(); r++) {
			const int lastX = (r == sel.Main()) ? lastXChosen : -1;
			const SelectionPosition spCaretNow = sel.Range(r).caret;
			const SelectionPosition posNew = MovePositionSoVisible(
				PositionUpOrDown(spCaretNow, direction, lastX), direction);
			sel.Range(r) = selt == Selection::SelTypes::stream ?
				SelectionRange(posNew, sel.Range(r).anchor) : SelectionRange(posNew);
		}
		sel.RemoveDuplicates();
		MovedCaret(sel.RangeMain().caret, caretToUse, true, caretPolicies);
	}
}

void Editor::ParaUpOrDown(int direction, Selection::SelTypes selt) {
	Sci::Line lineDoc;
	const Sci::Position savedPos = sel.MainCaret();
	do {
		MovePositionTo(SelectionPosition(direction > 0 ? pdoc->ParaDown(sel.MainCaret()) : pdoc->ParaUp(sel.MainCaret())), selt);
		lineDoc = pdoc->SciLineFromPosition(sel.MainCaret());
		if (direction > 0) {
			if (sel.MainCaret() >= pdoc->Length() && !pcs->GetVisible(lineDoc)) {
				if (selt == Selection::SelTypes::none) {
					MovePositionTo(SelectionPosition(pdoc->LineEndPosition(savedPos)));
				}
				break;
			}
		}
	} while (!pcs->GetVisible(lineDoc));
}

Range Editor::RangeDisplayLine(Sci::Line lineVisible) {
	RefreshStyleData();
	AutoSurface surface(this);
	return view.RangeDisplayLine(surface, *this, lineVisible, vs);
}

Sci::Position Editor::StartEndDisplayLine(Sci::Position pos, bool start) {
	RefreshStyleData();
	AutoSurface surface(this);
	const Sci::Position posRet = view.StartEndDisplayLine(surface, *this, pos, start, vs);
	if (posRet == INVALID_POSITION) {
		return pos;
	} else {
		return posRet;
	}
}

namespace {

constexpr short HighShortFromWParam(uptr_t x) noexcept {
	return static_cast<short>(x >> 16);
}

constexpr short LowShortFromWParam(uptr_t x) noexcept {
	return static_cast<short>(x & 0xffff);
}

constexpr unsigned int WithExtends(unsigned int iMessage) noexcept {
	switch (iMessage) {
	case SCI_CHARLEFT: return SCI_CHARLEFTEXTEND;
	case SCI_CHARRIGHT: return SCI_CHARRIGHTEXTEND;

	case SCI_WORDLEFT: return SCI_WORDLEFTEXTEND;
	case SCI_WORDRIGHT: return SCI_WORDRIGHTEXTEND;
	case SCI_WORDLEFTEND: return SCI_WORDLEFTENDEXTEND;
	case SCI_WORDRIGHTEND: return SCI_WORDRIGHTENDEXTEND;
	case SCI_WORDPARTLEFT: return SCI_WORDPARTLEFTEXTEND;
	case SCI_WORDPARTRIGHT: return SCI_WORDPARTRIGHTEXTEND;

	case SCI_HOME: return SCI_HOMEEXTEND;
	case SCI_HOMEDISPLAY: return SCI_HOMEDISPLAYEXTEND;
	case SCI_HOMEWRAP: return SCI_HOMEWRAPEXTEND;
	case SCI_VCHOME: return SCI_VCHOMEEXTEND;
	case SCI_VCHOMEDISPLAY: return SCI_VCHOMEDISPLAYEXTEND;
	case SCI_VCHOMEWRAP: return SCI_VCHOMEWRAPEXTEND;

	case SCI_LINEEND: return SCI_LINEENDEXTEND;
	case SCI_LINEENDDISPLAY: return SCI_LINEENDDISPLAYEXTEND;
	case SCI_LINEENDWRAP: return SCI_LINEENDWRAPEXTEND;

	default:	return iMessage;
	}
}

constexpr int NaturalDirection(unsigned int iMessage) noexcept {
	switch (iMessage) {
	case SCI_CHARLEFT:
	case SCI_CHARLEFTEXTEND:
	case SCI_CHARLEFTRECTEXTEND:
	case SCI_WORDLEFT:
	case SCI_WORDLEFTEXTEND:
	case SCI_WORDLEFTEND:
	case SCI_WORDLEFTENDEXTEND:
	case SCI_WORDPARTLEFT:
	case SCI_WORDPARTLEFTEXTEND:
	case SCI_HOME:
	case SCI_HOMEEXTEND:
	case SCI_HOMEDISPLAY:
	case SCI_HOMEDISPLAYEXTEND:
	case SCI_HOMEWRAP:
	case SCI_HOMEWRAPEXTEND:
		// VC_HOME* mostly goes back
	case SCI_VCHOME:
	case SCI_VCHOMEEXTEND:
	case SCI_VCHOMEDISPLAY:
	case SCI_VCHOMEDISPLAYEXTEND:
	case SCI_VCHOMEWRAP:
	case SCI_VCHOMEWRAPEXTEND:
		return -1;

	default:
		return 1;
	}
}

constexpr bool IsRectExtend(unsigned int iMessage, bool isRectMoveExtends) noexcept {
	switch (iMessage) {
	case SCI_CHARLEFTRECTEXTEND:
	case SCI_CHARRIGHTRECTEXTEND:
	case SCI_HOMERECTEXTEND:
	case SCI_VCHOMERECTEXTEND:
	case SCI_LINEENDRECTEXTEND:
		return true;
	default:
		if (isRectMoveExtends) {
			// Handle SCI_SETSELECTIONMODE(SC_SEL_RECTANGLE) and subsequent movements.
			switch (iMessage) {
			case SCI_CHARLEFTEXTEND:
			case SCI_CHARRIGHTEXTEND:
			case SCI_HOMEEXTEND:
			case SCI_VCHOMEEXTEND:
			case SCI_LINEENDEXTEND:
				return true;
			default:
				return false;
			}
		}
		return false;
	}
}

}

Sci::Position Editor::VCHomeDisplayPosition(Sci::Position position) {
	const Sci::Position homePos = pdoc->VCHomePosition(position);
	const Sci::Position viewLineStart = StartEndDisplayLine(position, true);
	if (viewLineStart > homePos)
		return viewLineStart;
	else
		return homePos;
}

Sci::Position Editor::VCHomeWrapPosition(Sci::Position position) {
	const Sci::Position homePos = pdoc->VCHomePosition(position);
	const Sci::Position viewLineStart = StartEndDisplayLine(position, true);
	if ((viewLineStart < position) && (viewLineStart > homePos))
		return viewLineStart;
	else
		return homePos;
}

Sci::Position Editor::LineEndWrapPosition(Sci::Position position) {
	const Sci::Position endPos = StartEndDisplayLine(position, false);
	const Sci::Position realEndPos = pdoc->LineEndPosition(position);
	if (endPos > realEndPos      // if moved past visible EOLs
		|| position >= endPos) // if at end of display line already
		return realEndPos;
	else
		return endPos;
}

int Editor::HorizontalMove(unsigned int iMessage) {
	if (sel.selType == Selection::SelTypes::lines) {
		return 0; // horizontal moves with line selection have no effect
	}
	if (sel.MoveExtends()) {
		iMessage = WithExtends(iMessage);
	}

	if (!multipleSelection && !sel.IsRectangular()) {
		// Simplify selection down to 1
		sel.SetSelection(sel.RangeMain());
	}

	// Invalidate each of the current selections
	InvalidateWholeSelection();

	if (IsRectExtend(iMessage, sel.IsRectangular() && sel.MoveExtends())) {
		const SelectionRange rangeBase = sel.IsRectangular() ? sel.Rectangular() : sel.RangeMain();
		if (!sel.IsRectangular()) {
			sel.DropAdditionalRanges();
		}
		// Will change to rectangular if not currently rectangular
		SelectionPosition spCaret = rangeBase.caret;
		switch (iMessage) {
		case SCI_CHARLEFTRECTEXTEND:
		case SCI_CHARLEFTEXTEND: // only when sel.IsRectangular() && sel.MoveExtends()
			if (pdoc->IsLineEndPosition(spCaret.Position()) && spCaret.VirtualSpace()) {
				spCaret.SetVirtualSpace(spCaret.VirtualSpace() - 1);
			} else if ((virtualSpaceOptions & SCVS_NOWRAPLINESTART) == 0 || pdoc->GetColumn(spCaret.Position()) > 0) {
				spCaret = SelectionPosition(spCaret.Position() - 1);
			}
			break;
		case SCI_CHARRIGHTRECTEXTEND:
		case SCI_CHARRIGHTEXTEND: // only when sel.IsRectangular() && sel.MoveExtends()
			if ((virtualSpaceOptions & SCVS_RECTANGULARSELECTION) && pdoc->IsLineEndPosition(sel.MainCaret())) {
				spCaret.SetVirtualSpace(spCaret.VirtualSpace() + 1);
			} else {
				spCaret = SelectionPosition(spCaret.Position() + 1);
			}
			break;
		case SCI_HOMERECTEXTEND:
		case SCI_HOMEEXTEND: // only when sel.IsRectangular() && sel.MoveExtends()
			spCaret = SelectionPosition(
				pdoc->LineStart(pdoc->LineFromPosition(spCaret.Position())));
			break;
		case SCI_VCHOMERECTEXTEND:
		case SCI_VCHOMEEXTEND: // only when sel.IsRectangular() && sel.MoveExtends()
			spCaret = SelectionPosition(pdoc->VCHomePosition(spCaret.Position()));
			break;
		case SCI_LINEENDRECTEXTEND:
		case SCI_LINEENDEXTEND: // only when sel.IsRectangular() && sel.MoveExtends()
			spCaret = SelectionPosition(pdoc->LineEndPosition(spCaret.Position()));
			break;
		}
		const int directionMove = (spCaret < rangeBase.caret) ? -1 : 1;
		spCaret = MovePositionSoVisible(spCaret, directionMove);
		sel.selType = Selection::SelTypes::rectangle;
		sel.Rectangular() = SelectionRange(spCaret, rangeBase.anchor);
		SetRectangularRange();
	} else if (sel.IsRectangular()) {
		// Not a rectangular extension so switch to stream.
		SelectionPosition selAtLimit = (NaturalDirection(iMessage) > 0) ? sel.Limits().end : sel.Limits().start;
		switch (iMessage) {
		case SCI_HOME:
			selAtLimit = SelectionPosition(
				pdoc->LineStart(pdoc->LineFromPosition(selAtLimit.Position())));
			break;
		case SCI_VCHOME:
			selAtLimit = SelectionPosition(pdoc->VCHomePosition(selAtLimit.Position()));
			break;
		case SCI_LINEEND:
			selAtLimit = SelectionPosition(pdoc->LineEndPosition(selAtLimit.Position()));
			break;
		}
		sel.selType = Selection::SelTypes::stream;
		sel.SetSelection(SelectionRange(selAtLimit));
	} else {
		if (!additionalSelectionTyping) {
			InvalidateWholeSelection();
			sel.DropAdditionalRanges();
		}
		for (size_t r = 0; r < sel.Count(); r++) {
			const SelectionPosition spCaretNow = sel.Range(r).caret;
			SelectionPosition spCaret = spCaretNow;
			switch (iMessage) {
			case SCI_CHARLEFT:
			case SCI_CHARLEFTEXTEND:
				if (spCaret.VirtualSpace()) {
					spCaret.SetVirtualSpace(spCaret.VirtualSpace() - 1);
				} else if ((virtualSpaceOptions & SCVS_NOWRAPLINESTART) == 0 || pdoc->GetColumn(spCaret.Position()) > 0) {
					spCaret = SelectionPosition(spCaret.Position() - 1);
				}
				break;
			case SCI_CHARRIGHT:
			case SCI_CHARRIGHTEXTEND:
				if ((virtualSpaceOptions & SCVS_USERACCESSIBLE) && pdoc->IsLineEndPosition(spCaret.Position())) {
					spCaret.SetVirtualSpace(spCaret.VirtualSpace() + 1);
				} else {
					spCaret = SelectionPosition(spCaret.Position() + 1);
				}
				break;
			case SCI_WORDLEFT:
			case SCI_WORDLEFTEXTEND:
				spCaret = SelectionPosition(pdoc->NextWordStart(spCaret.Position(), -1));
				break;
			case SCI_WORDRIGHT:
			case SCI_WORDRIGHTEXTEND:
				spCaret = SelectionPosition(pdoc->NextWordStart(spCaret.Position(), 1));
				break;
			case SCI_WORDLEFTEND:
			case SCI_WORDLEFTENDEXTEND:
				spCaret = SelectionPosition(pdoc->NextWordEnd(spCaret.Position(), -1));
				break;
			case SCI_WORDRIGHTEND:
			case SCI_WORDRIGHTENDEXTEND:
				spCaret = SelectionPosition(pdoc->NextWordEnd(spCaret.Position(), 1));
				break;
			case SCI_WORDPARTLEFT:
			case SCI_WORDPARTLEFTEXTEND:
				spCaret = SelectionPosition(pdoc->WordPartLeft(spCaret.Position()));
				break;
			case SCI_WORDPARTRIGHT:
			case SCI_WORDPARTRIGHTEXTEND:
				spCaret = SelectionPosition(pdoc->WordPartRight(spCaret.Position()));
				break;
			case SCI_HOME:
			case SCI_HOMEEXTEND:
				spCaret = SelectionPosition(
					pdoc->LineStart(pdoc->LineFromPosition(spCaret.Position())));
				break;
			case SCI_HOMEDISPLAY:
			case SCI_HOMEDISPLAYEXTEND:
				spCaret = SelectionPosition(StartEndDisplayLine(spCaret.Position(), true));
				break;
			case SCI_HOMEWRAP:
			case SCI_HOMEWRAPEXTEND:
				spCaret = MovePositionSoVisible(StartEndDisplayLine(spCaret.Position(), true), -1);
				if (spCaretNow <= spCaret)
					spCaret = SelectionPosition(
						pdoc->LineStart(pdoc->LineFromPosition(spCaret.Position())));
				break;
			case SCI_VCHOME:
			case SCI_VCHOMEEXTEND:
				// VCHome alternates between beginning of line and beginning of text so may move back or forwards
				spCaret = SelectionPosition(pdoc->VCHomePosition(spCaret.Position()));
				break;
			case SCI_VCHOMEDISPLAY:
			case SCI_VCHOMEDISPLAYEXTEND:
				spCaret = SelectionPosition(VCHomeDisplayPosition(spCaret.Position()));
				break;
			case SCI_VCHOMEWRAP:
			case SCI_VCHOMEWRAPEXTEND:
				spCaret = SelectionPosition(VCHomeWrapPosition(spCaret.Position()));
				break;
			case SCI_LINEEND:
			case SCI_LINEENDEXTEND:
				spCaret = SelectionPosition(pdoc->LineEndPosition(spCaret.Position()));
				break;
			case SCI_LINEENDDISPLAY:
			case SCI_LINEENDDISPLAYEXTEND:
				spCaret = SelectionPosition(StartEndDisplayLine(spCaret.Position(), false));
				break;
			case SCI_LINEENDWRAP:
			case SCI_LINEENDWRAPEXTEND:
				spCaret = SelectionPosition(LineEndWrapPosition(spCaret.Position()));
				break;

			default:
				PLATFORM_ASSERT(false);
			}

			const int directionMove = (spCaret < spCaretNow) ? -1 : 1;
			spCaret = MovePositionSoVisible(spCaret, directionMove);

			// Handle move versus extend, and special behaviour for non-empty left/right
			switch (iMessage) {
			case SCI_CHARLEFT:
			case SCI_CHARRIGHT:
				if (sel.Range(r).Empty()) {
					sel.Range(r) = SelectionRange(spCaret);
				} else {
					sel.Range(r) = SelectionRange(
						(iMessage == SCI_CHARLEFT) ? sel.Range(r).Start() : sel.Range(r).End());
				}
				break;

			case SCI_WORDLEFT:
			case SCI_WORDRIGHT:
			case SCI_WORDLEFTEND:
			case SCI_WORDRIGHTEND:
			case SCI_WORDPARTLEFT:
			case SCI_WORDPARTRIGHT:
			case SCI_HOME:
			case SCI_HOMEDISPLAY:
			case SCI_HOMEWRAP:
			case SCI_VCHOME:
			case SCI_VCHOMEDISPLAY:
			case SCI_VCHOMEWRAP:
			case SCI_LINEEND:
			case SCI_LINEENDDISPLAY:
			case SCI_LINEENDWRAP:
				sel.Range(r) = SelectionRange(spCaret);
				break;

			case SCI_CHARLEFTEXTEND:
			case SCI_CHARRIGHTEXTEND:
			case SCI_WORDLEFTEXTEND:
			case SCI_WORDRIGHTEXTEND:
			case SCI_WORDLEFTENDEXTEND:
			case SCI_WORDRIGHTENDEXTEND:
			case SCI_WORDPARTLEFTEXTEND:
			case SCI_WORDPARTRIGHTEXTEND:
			case SCI_HOMEEXTEND:
			case SCI_HOMEDISPLAYEXTEND:
			case SCI_HOMEWRAPEXTEND:
			case SCI_VCHOMEEXTEND:
			case SCI_VCHOMEDISPLAYEXTEND:
			case SCI_VCHOMEWRAPEXTEND:
			case SCI_LINEENDEXTEND:
			case SCI_LINEENDDISPLAYEXTEND:
			case SCI_LINEENDWRAPEXTEND: {
				SelectionRange rangeNew = SelectionRange(spCaret, sel.Range(r).anchor);
				sel.TrimOtherSelections(r, SelectionRange(rangeNew));
				sel.Range(r) = rangeNew;
			}
			break;

			default:
				PLATFORM_ASSERT(false);
			}
		}
	}

	sel.RemoveDuplicates();

	MovedCaret(sel.RangeMain().caret, SelectionPosition(INVALID_POSITION), true, caretPolicies);

	// Invalidate the new state of the selection
	InvalidateWholeSelection();

	SetLastXChosen();
	// Need the line moving and so forth from MovePositionTo
	return 0;
}

int Editor::DelWordOrLine(unsigned int iMessage) {
	// Virtual space may be realised for SCI_DELWORDRIGHT or SCI_DELWORDRIGHTEND
	// which means 2 actions so wrap in an undo group.

	// Rightwards and leftwards deletions differ in treatment of virtual space.
	// Clear virtual space for leftwards, realise for rightwards.
	const bool leftwards = (iMessage == SCI_DELWORDLEFT) || (iMessage == SCI_DELLINELEFT);

	if (!additionalSelectionTyping) {
		InvalidateWholeSelection();
		sel.DropAdditionalRanges();
	}

	UndoGroup ug0(pdoc, (sel.Count() > 1) || !leftwards);

	for (size_t r = 0; r < sel.Count(); r++) {
		if (leftwards) {
			// Delete to the left so first clear the virtual space.
			sel.Range(r).ClearVirtualSpace();
		} else {
			// Delete to the right so first realise the virtual space.
			sel.Range(r) = SelectionRange(
				RealizeVirtualSpace(sel.Range(r).caret));
		}

		Range rangeDelete;
		const Sci::Position caretPosition = sel.Range(r).caret.Position();
		switch (iMessage) {
		case SCI_DELWORDLEFT:
			rangeDelete = Range(pdoc->NextWordStart(caretPosition, -1), caretPosition);
			break;
		case SCI_DELWORDRIGHT:
			rangeDelete = Range(caretPosition, pdoc->NextWordStart(caretPosition, 1));
			break;
		case SCI_DELWORDRIGHTEND:
			rangeDelete = Range(caretPosition, pdoc->NextWordEnd(caretPosition, 1));
			break;
		case SCI_DELLINELEFT:
			rangeDelete = Range(pdoc->LineStart(pdoc->LineFromPosition(caretPosition)), caretPosition);
			break;
		case SCI_DELLINERIGHT:
			rangeDelete = Range(caretPosition, pdoc->LineEnd(pdoc->LineFromPosition(caretPosition)));
			break;
		}
		if (!RangeContainsProtected(rangeDelete.start, rangeDelete.end)) {
			pdoc->DeleteChars(rangeDelete.start, rangeDelete.end - rangeDelete.start);
		}
	}

	// May need something stronger here: can selections overlap at this point?
	sel.RemoveDuplicates();

	MovedCaret(sel.RangeMain().caret, SelectionPosition(INVALID_POSITION), true, caretPolicies);

	// Invalidate the new state of the selection
	InvalidateWholeSelection();

	SetLastXChosen();
	return 0;
}

int Editor::KeyCommand(unsigned int iMessage) {
	switch (iMessage) {
	case SCI_LINEDOWN:
		CursorUpOrDown(1, Selection::SelTypes::none);
		break;
	case SCI_LINEDOWNEXTEND:
		CursorUpOrDown(1, Selection::SelTypes::stream);
		break;
	case SCI_LINEDOWNRECTEXTEND:
		CursorUpOrDown(1, Selection::SelTypes::rectangle);
		break;
	case SCI_PARADOWN:
		ParaUpOrDown(1, Selection::SelTypes::none);
		break;
	case SCI_PARADOWNEXTEND:
		ParaUpOrDown(1, Selection::SelTypes::stream);
		break;
	case SCI_LINESCROLLDOWN:
		ScrollTo(topLine + 1);
		MoveCaretInsideView(false);
		break;
	case SCI_LINEUP:
		CursorUpOrDown(-1, Selection::SelTypes::none);
		break;
	case SCI_LINEUPEXTEND:
		CursorUpOrDown(-1, Selection::SelTypes::stream);
		break;
	case SCI_LINEUPRECTEXTEND:
		CursorUpOrDown(-1, Selection::SelTypes::rectangle);
		break;
	case SCI_PARAUP:
		ParaUpOrDown(-1, Selection::SelTypes::none);
		break;
	case SCI_PARAUPEXTEND:
		ParaUpOrDown(-1, Selection::SelTypes::stream);
		break;
	case SCI_LINESCROLLUP:
		ScrollTo(topLine - 1);
		MoveCaretInsideView(false);
		break;

	case SCI_CHARLEFT:
	case SCI_CHARLEFTEXTEND:
	case SCI_CHARLEFTRECTEXTEND:
	case SCI_CHARRIGHT:
	case SCI_CHARRIGHTEXTEND:
	case SCI_CHARRIGHTRECTEXTEND:
	case SCI_WORDLEFT:
	case SCI_WORDLEFTEXTEND:
	case SCI_WORDRIGHT:
	case SCI_WORDRIGHTEXTEND:
	case SCI_WORDLEFTEND:
	case SCI_WORDLEFTENDEXTEND:
	case SCI_WORDRIGHTEND:
	case SCI_WORDRIGHTENDEXTEND:
	case SCI_WORDPARTLEFT:
	case SCI_WORDPARTLEFTEXTEND:
	case SCI_WORDPARTRIGHT:
	case SCI_WORDPARTRIGHTEXTEND:
	case SCI_HOME:
	case SCI_HOMEEXTEND:
	case SCI_HOMERECTEXTEND:
	case SCI_HOMEDISPLAY:
	case SCI_HOMEDISPLAYEXTEND:
	case SCI_HOMEWRAP:
	case SCI_HOMEWRAPEXTEND:
	case SCI_VCHOME:
	case SCI_VCHOMEEXTEND:
	case SCI_VCHOMERECTEXTEND:
	case SCI_VCHOMEDISPLAY:
	case SCI_VCHOMEDISPLAYEXTEND:
	case SCI_VCHOMEWRAP:
	case SCI_VCHOMEWRAPEXTEND:
	case SCI_LINEEND:
	case SCI_LINEENDEXTEND:
	case SCI_LINEENDRECTEXTEND:
	case SCI_LINEENDDISPLAY:
	case SCI_LINEENDDISPLAYEXTEND:
	case SCI_LINEENDWRAP:
	case SCI_LINEENDWRAPEXTEND:
		return HorizontalMove(iMessage);

	case SCI_DOCUMENTSTART:
		MovePositionTo(0);
		SetLastXChosen();
		break;
	case SCI_DOCUMENTSTARTEXTEND:
		MovePositionTo(0, Selection::SelTypes::stream);
		SetLastXChosen();
		break;
	case SCI_DOCUMENTEND:
		MovePositionTo(pdoc->Length());
		SetLastXChosen();
		break;
	case SCI_DOCUMENTENDEXTEND:
		MovePositionTo(pdoc->Length(), Selection::SelTypes::stream);
		SetLastXChosen();
		break;
	case SCI_STUTTEREDPAGEUP:
		PageMove(-1, Selection::SelTypes::none, true);
		break;
	case SCI_STUTTEREDPAGEUPEXTEND:
		PageMove(-1, Selection::SelTypes::stream, true);
		break;
	case SCI_STUTTEREDPAGEDOWN:
		PageMove(1, Selection::SelTypes::none, true);
		break;
	case SCI_STUTTEREDPAGEDOWNEXTEND:
		PageMove(1, Selection::SelTypes::stream, true);
		break;
	case SCI_PAGEUP:
		PageMove(-1);
		break;
	case SCI_PAGEUPEXTEND:
		PageMove(-1, Selection::SelTypes::stream);
		break;
	case SCI_PAGEUPRECTEXTEND:
		PageMove(-1, Selection::SelTypes::rectangle);
		break;
	case SCI_PAGEDOWN:
		PageMove(1);
		break;
	case SCI_PAGEDOWNEXTEND:
		PageMove(1, Selection::SelTypes::stream);
		break;
	case SCI_PAGEDOWNRECTEXTEND:
		PageMove(1, Selection::SelTypes::rectangle);
		break;
	case SCI_EDITTOGGLEOVERTYPE:
		inOverstrike = !inOverstrike;
		ContainerNeedsUpdate(SC_UPDATE_SELECTION);
		ShowCaretAtCurrentPosition();
		SetIdle(true);
		break;
	case SCI_CANCEL:            	// Cancel any modes - handled in subclass
		// Also unselect text
		CancelModes();
		if ((sel.Count() > 1) && !sel.IsRectangular()) {
			// Drop additional selections
			InvalidateWholeSelection();
			sel.DropAdditionalRanges();
		}
		break;
	case SCI_DELETEBACK:
		DelCharBack(true);
		if ((caretSticky == SC_CARETSTICKY_OFF) || (caretSticky == SC_CARETSTICKY_WHITESPACE)) {
			SetLastXChosen();
		}
		EnsureCaretVisible();
		break;
	case SCI_DELETEBACKNOTLINE:
		DelCharBack(false);
		if ((caretSticky == SC_CARETSTICKY_OFF) || (caretSticky == SC_CARETSTICKY_WHITESPACE)) {
			SetLastXChosen();
		}
		EnsureCaretVisible();
		break;
	case SCI_TAB:
		Indent(true);
		if (caretSticky == SC_CARETSTICKY_OFF) {
			SetLastXChosen();
		}
		EnsureCaretVisible();
		ShowCaretAtCurrentPosition();		// Avoid blinking
		break;
	case SCI_BACKTAB:
		Indent(false);
		if ((caretSticky == SC_CARETSTICKY_OFF) || (caretSticky == SC_CARETSTICKY_WHITESPACE)) {
			SetLastXChosen();
		}
		EnsureCaretVisible();
		ShowCaretAtCurrentPosition();		// Avoid blinking
		break;
	case SCI_NEWLINE:
		NewLine();
		break;
	case SCI_FORMFEED:
		AddChar('\f');
		break;
	case SCI_ZOOMIN:
		if (vs.ZoomIn()) {
			InvalidateStyleRedraw();
			NotifyZoom();
		}
		break;
	case SCI_ZOOMOUT:
		if (vs.ZoomOut()) {
			InvalidateStyleRedraw();
			NotifyZoom();
		}
		break;

	case SCI_DELWORDLEFT:
	case SCI_DELWORDRIGHT:
	case SCI_DELWORDRIGHTEND:
	case SCI_DELLINELEFT:
	case SCI_DELLINERIGHT:
		return DelWordOrLine(iMessage);

	case SCI_LINEDELETE: {
		const Sci::Line line = pdoc->SciLineFromPosition(sel.MainCaret());
		const Sci::Position start = pdoc->LineStart(line);
		const Sci::Position end = pdoc->LineStart(line + 1);
		pdoc->DeleteChars(start, end - start);
	}
	break;
	case SCI_LINETRANSPOSE:
		LineTranspose();
		break;
	case SCI_LINEREVERSE:
		LineReverse();
		break;
	case SCI_LINEDUPLICATE:
		Duplicate(true);
		break;
	case SCI_SELECTIONDUPLICATE:
		Duplicate(false);
		break;
	case SCI_LOWERCASE:
		ChangeCaseOfSelection(CaseMapping::lower);
		break;
	case SCI_UPPERCASE:
		ChangeCaseOfSelection(CaseMapping::upper);
		break;
	case SCI_SCROLLTOSTART:
		ScrollTo(0);
		break;
	case SCI_SCROLLTOEND:
		ScrollTo(MaxScrollPos());
		break;
	}
	return 0;
}

int Editor::KeyDefault(int, int) noexcept {
	return 0;
}

int Editor::KeyDownWithModifiers(int key, int modifiers, bool *consumed) {
	DwellEnd(false);
	const unsigned int msg = kmap.Find(key, modifiers);
	if (msg) {
		if (consumed)
			*consumed = true;
		return static_cast<int>(WndProc(msg, 0, 0));
	} else {
		if (consumed)
			*consumed = false;
		return KeyDefault(key, modifiers);
	}
}

void Editor::Indent(bool forwards) {
	UndoGroup ug(pdoc);
	for (size_t r = 0; r < sel.Count(); r++) {
		const Sci::Line lineOfAnchor =
			pdoc->SciLineFromPosition(sel.Range(r).anchor.Position());
		Sci::Position caretPosition = sel.Range(r).caret.Position();
		const Sci::Line lineCurrentPos = pdoc->SciLineFromPosition(caretPosition);
		if (lineOfAnchor == lineCurrentPos) {
			if (forwards) {
				pdoc->DeleteChars(sel.Range(r).Start().Position(), sel.Range(r).Length());
				caretPosition = sel.Range(r).caret.Position();
				if (pdoc->tabIndents && pdoc->GetColumn(caretPosition) <= pdoc->GetColumn(pdoc->GetLineIndentPosition(lineCurrentPos))) {
					const int indentation = pdoc->GetLineIndentation(lineCurrentPos);
					const int indentationStep = pdoc->IndentSize();
					const Sci::Position posSelect = pdoc->SetLineIndentation(
						lineCurrentPos, indentation + indentationStep - indentation % indentationStep);
					sel.Range(r) = SelectionRange(posSelect);
				} else {
					if (pdoc->useTabs) {
						const Sci::Position lengthInserted = pdoc->InsertString(caretPosition, "\t", 1);
						sel.Range(r) = SelectionRange(caretPosition + lengthInserted);
					} else {
						int numSpaces = (pdoc->tabInChars) -
							(pdoc->GetColumn(caretPosition) % (pdoc->tabInChars));
						if (numSpaces < 1)
							numSpaces = pdoc->tabInChars;
						const std::string spaceText(numSpaces, ' ');
						const Sci::Position lengthInserted = pdoc->InsertString(caretPosition, spaceText.c_str(),
							spaceText.length());
						sel.Range(r) = SelectionRange(caretPosition + lengthInserted);
					}
				}
			} else {
				const Sci::Position column = pdoc->GetColumn(caretPosition);
				const int indentation = pdoc->tabIndents ? pdoc->GetLineIndentation(lineCurrentPos) : -1;
				if (column <= indentation) {
					const int indentationStep = pdoc->IndentSize();
					const Sci::Position posSelect = pdoc->SetLineIndentation(lineCurrentPos, indentation - indentationStep);
					sel.Range(r) = SelectionRange(posSelect);
				} else {
					Sci::Position newColumn = ((column - 1) / pdoc->tabInChars) *
						pdoc->tabInChars;
					newColumn = std::max<Sci::Position>(newColumn, 0);
					Sci::Position newPos = caretPosition;
					while (pdoc->GetColumn(newPos) > newColumn)
						newPos--;
					sel.Range(r) = SelectionRange(newPos);
				}
			}
		} else {	// Multiline
			const Sci::Position anchorPosOnLine = sel.Range(r).anchor.Position() -
				pdoc->LineStart(lineOfAnchor);
			const Sci::Position currentPosPosOnLine = caretPosition -
				pdoc->LineStart(lineCurrentPos);
			// Multiple lines selected so indent / dedent
			const Sci::Line lineTopSel = std::min(lineOfAnchor, lineCurrentPos);
			Sci::Line lineBottomSel = std::max(lineOfAnchor, lineCurrentPos);
			if (pdoc->LineStart(lineBottomSel) == sel.Range(r).anchor.Position() || pdoc->LineStart(lineBottomSel) == caretPosition)
				lineBottomSel--;  	// If not selecting any characters on a line, do not indent
			pdoc->Indent(forwards, lineBottomSel, lineTopSel);
			if (lineOfAnchor < lineCurrentPos) {
				if (currentPosPosOnLine == 0)
					sel.Range(r) = SelectionRange(pdoc->LineStart(lineCurrentPos),
						pdoc->LineStart(lineOfAnchor));
				else
					sel.Range(r) = SelectionRange(pdoc->LineStart(lineCurrentPos + 1),
						pdoc->LineStart(lineOfAnchor));
			} else {
				if (anchorPosOnLine == 0)
					sel.Range(r) = SelectionRange(pdoc->LineStart(lineCurrentPos),
						pdoc->LineStart(lineOfAnchor));
				else
					sel.Range(r) = SelectionRange(pdoc->LineStart(lineCurrentPos),
						pdoc->LineStart(lineOfAnchor + 1));
			}
		}
	}
	ContainerNeedsUpdate(SC_UPDATE_SELECTION);
}

std::unique_ptr<CaseFolder> Editor::CaseFolderForEncoding() {
	// Simple default that only maps ASCII upper case to lower case.
	return std::make_unique<CaseFolderTable>();
}

/**
 * Search of a text in the document, in the given range.
 * @return The position of the found text, -1 if not found.
 */
Sci::Position Editor::FindText(
	uptr_t wParam,		///< Search modes : @c SCFIND_MATCHCASE, @c SCFIND_WHOLEWORD,
	///< @c SCFIND_WORDSTART, @c SCFIND_REGEXP or @c SCFIND_POSIX.
	sptr_t lParam) {	///< @c Sci_TextToFind structure: The text to search for in the given range.

	Sci_TextToFind *ft = static_cast<Sci_TextToFind *>(PtrFromSPtr(lParam));
	Sci::Position lengthFound = strlen(ft->lpstrText);
	if (!pdoc->HasCaseFolder())
		pdoc->SetCaseFolder(CaseFolderForEncoding());
	try {
		const Sci::Position pos = pdoc->FindText(
			ft->chrg.cpMin,
			ft->chrg.cpMax,
			ft->lpstrText,
			static_cast<int>(wParam),
			&lengthFound);
		if (pos != -1) {
			ft->chrgText.cpMin = pos;
			ft->chrgText.cpMax = pos + lengthFound;
		}
		return pos;
	} catch (const RegexError &) {
		errorStatus = SC_STATUS_WARN_REGEX;
		return -1;
	}
}

/**
 * Relocatable search support : Searches relative to current selection
 * point and sets the selection to the found text range with
 * each search.
 */
/**
 * Anchor following searches at current selection start: This allows
 * multiple incremental interactive searches to be macro recorded
 * while still setting the selection to found text so the find/select
 * operation is self-contained.
 */
void Editor::SearchAnchor() noexcept {
	searchAnchor = SelectionStart().Position();
}

/**
 * Find text from current search anchor: Must call @c SearchAnchor first.
 * Used for next text and previous text requests.
 * @return The position of the found text, -1 if not found.
 */
Sci::Position Editor::SearchText(
	unsigned int iMessage,		///< Accepts both @c SCI_SEARCHNEXT and @c SCI_SEARCHPREV.
	uptr_t wParam,				///< Search modes : @c SCFIND_MATCHCASE, @c SCFIND_WHOLEWORD,
	///< @c SCFIND_WORDSTART, @c SCFIND_REGEXP or @c SCFIND_POSIX.
	sptr_t lParam) {			///< The text to search for.

	const char *txt = CharPtrFromSPtr(lParam);
	Sci::Position pos = INVALID_POSITION;
	Sci::Position lengthFound = strlen(txt);
	if (!pdoc->HasCaseFolder())
		pdoc->SetCaseFolder(CaseFolderForEncoding());
	try {
		if (iMessage == SCI_SEARCHNEXT) {
			pos = pdoc->FindText(searchAnchor, pdoc->Length(), txt,
				static_cast<int>(wParam),
				&lengthFound);
		} else {
			pos = pdoc->FindText(searchAnchor, 0, txt,
				static_cast<int>(wParam),
				&lengthFound);
		}
	} catch (const RegexError &) {
		errorStatus = SC_STATUS_WARN_REGEX;
		return INVALID_POSITION;
	}
	if (pos != INVALID_POSITION) {
		SetSelection(pos, pos + lengthFound);
	}

	return pos;
}

std::string Editor::CaseMapString(const std::string &s, CaseMapping caseMapping) {
	std::string ret(s);
	for (char &ch : ret) {
		switch (caseMapping) {
		case CaseMapping::upper:
			ch = MakeUpperCase(ch);
			break;
		case CaseMapping::lower:
			ch = MakeLowerCase(ch);
			break;
		default:	// no action
			break;
		}
	}
	return ret;
}

/**
 * Search for text in the target range of the document.
 * @return The position of the found text, -1 if not found.
 */
Sci::Position Editor::SearchInTarget(const char *text, Sci::Position length) {
	Sci::Position lengthFound = length;

	if (!pdoc->HasCaseFolder())
		pdoc->SetCaseFolder(CaseFolderForEncoding());
	try {
		const Sci::Position pos = pdoc->FindText(targetRange.start.Position(), targetRange.end.Position(), text,
			searchFlags,
			&lengthFound);
		if (pos != -1) {
			targetRange.start.SetPosition(pos);
			targetRange.end.SetPosition(pos + lengthFound);
		}
		return pos;
	} catch (const RegexError &) {
		errorStatus = SC_STATUS_WARN_REGEX;
		return -1;
	}
}

void Editor::GoToLine(Sci::Line lineNo) {
	lineNo = std::clamp<Sci::Line>(lineNo, 0, pdoc->LinesTotal());
	SetEmptySelection(pdoc->LineStart(lineNo));
	ShowCaretAtCurrentPosition();
	EnsureCaretVisible();
}

static bool Close(Point pt1, Point pt2, Point threshold) noexcept {
	const Point ptDifference = pt2 - pt1;
	if (std::abs(ptDifference.x) > threshold.x)
		return false;
	if (std::abs(ptDifference.y) > threshold.y)
		return false;
	return true;
}

std::string Editor::RangeText(Sci::Position start, Sci::Position end) const {
	if (start < end) {
		const Sci::Position len = end - start;
		std::string ret(len, '\0');
		pdoc->GetCharRange(ret.data(), start, len);
		return ret;
	}
	return {};
}

void Editor::CopySelectionRange(SelectionText *ss, bool allowLineCopy) {
	if (sel.Empty()) {
		if (allowLineCopy) {
			const Sci::Line currentLine = pdoc->SciLineFromPosition(sel.MainCaret());
			const Sci::Position start = pdoc->LineStart(currentLine);
			const Sci::Position end = pdoc->LineStart(currentLine + 1);

			std::string text = RangeText(start, end);
			ss->Copy(text, pdoc->dbcsCodePage, false, true);
		}
	} else {
		std::string text;
		std::vector<SelectionRange> rangesInOrder = sel.RangesCopy();
		if (sel.selType == Selection::SelTypes::rectangle)
			std::sort(rangesInOrder.begin(), rangesInOrder.end());
		for (const SelectionRange &current : rangesInOrder) {
			text.append(RangeText(current.Start().Position(), current.End().Position()));
			if (sel.selType == Selection::SelTypes::rectangle) {
				if (pdoc->eolMode != SC_EOL_LF)
					text.push_back('\r');
				if (pdoc->eolMode != SC_EOL_CR)
					text.push_back('\n');
			}
		}
		ss->Copy(text, pdoc->dbcsCodePage, sel.IsRectangular(), sel.selType == Selection::SelTypes::lines);
	}
}

void Editor::CopyRangeToClipboard(Sci::Position start, Sci::Position end, bool lineCopy) {
	start = pdoc->ClampPositionIntoDocument(start);
	end = pdoc->ClampPositionIntoDocument(end);
	SelectionText selectedText;
	std::string text = RangeText(start, end);
	selectedText.Copy(text, pdoc->dbcsCodePage, false, lineCopy);
	CopyToClipboard(selectedText);
}

void Editor::CopyText(size_t length, const char *text) {
	SelectionText selectedText;
	selectedText.Copy(std::string(text, length), pdoc->dbcsCodePage, false, false);
	CopyToClipboard(selectedText);
}

void Editor::SetDragPosition(SelectionPosition newPos) {
	if (newPos.Position() >= 0) {
		newPos = MovePositionOutsideChar(newPos, 1);
		posDrop = newPos;
	}
	if (!(posDrag == newPos)) {
		const CaretPolicies dragCaretPolicies = {
			{ CARET_SLOP | CARET_STRICT | CARET_EVEN, 50 },
			{ CARET_SLOP | CARET_STRICT | CARET_EVEN, 2 }
		};
		MovedCaret(newPos, posDrag, true, dragCaretPolicies);

		caret.on = true;
		FineTickerCancel(TickReason::caret);
		if ((caret.active) && (caret.period > 0) && (newPos.Position() < 0))
			FineTickerStart(TickReason::caret, caret.period, caret.period / 10);
		InvalidateCaret();
		posDrag = newPos;
		InvalidateCaret();
	}
}

void Editor::DisplayCursor(Window::Cursor c) noexcept {
	if (cursorMode == SC_CURSORNORMAL)
		wMain.SetCursor(c);
	else
		wMain.SetCursor(static_cast<Window::Cursor>(cursorMode));
}

bool Editor::DragThreshold(Point ptStart, Point ptNow) noexcept {
	const Point ptDiff = ptStart - ptNow;
	const XYPOSITION distanceSquared = ptDiff.x * ptDiff.x + ptDiff.y * ptDiff.y;
	return distanceSquared > 16.0f;
}

void Editor::StartDrag() {
	// Always handled by subclasses
	//SetMouseCapture(true);
	//DisplayCursor(Window::Cursor::::arrow);
}

void Editor::DropAt(SelectionPosition position, const char *value, size_t lengthValue, bool moving, bool rectangular) {
	//Platform::DebugPrintf("DropAt %d %d\n", inDragDrop, position);
	if (inDragDrop == DragDrop::dragging)
		dropWentOutside = false;

	const bool positionWasInSelection = PositionInSelection(position.Position());

	const bool positionOnEdgeOfSelection =
		(position == SelectionStart()) || (position == SelectionEnd());

	if ((inDragDrop != DragDrop::dragging) || !(positionWasInSelection) ||
		(positionOnEdgeOfSelection && !moving)) {

		const SelectionPosition selStart = SelectionStart();
		const SelectionPosition selEnd = SelectionEnd();

		UndoGroup ug(pdoc);

		SelectionPosition positionAfterDeletion = position;
		if ((inDragDrop == DragDrop::dragging) && moving) {
			// Remove dragged out text
			if (rectangular || sel.selType == Selection::SelTypes::lines) {
				for (size_t r = 0; r < sel.Count(); r++) {
					if (position >= sel.Range(r).Start()) {
						if (position > sel.Range(r).End()) {
							positionAfterDeletion.Add(-sel.Range(r).Length());
						} else {
							positionAfterDeletion.Add(-SelectionRange(position, sel.Range(r).Start()).Length());
						}
					}
				}
			} else {
				if (position > selStart) {
					positionAfterDeletion.Add(-SelectionRange(selEnd, selStart).Length());
				}
			}
			ClearSelection();
		}
		position = positionAfterDeletion;

		std::string convertedText = Document::TransformLineEnds(value, lengthValue, pdoc->eolMode);

		if (rectangular) {
			PasteRectangular(position, convertedText.c_str(), convertedText.length());
			// Should try to select new rectangle but it may not be a rectangle now so just select the drop position
			SetEmptySelection(position);
		} else {
			position = MovePositionOutsideChar(position, sel.MainCaret() - position.Position());
			position = RealizeVirtualSpace(position);
			const Sci::Position lengthInserted = pdoc->InsertString(
				position.Position(), convertedText.c_str(), convertedText.length());
			if (lengthInserted > 0) {
				SelectionPosition posAfterInsertion = position;
				posAfterInsertion.Add(lengthInserted);
				SetSelection(posAfterInsertion, position);
			}
		}
	} else if (inDragDrop == DragDrop::dragging) {
		SetEmptySelection(position);
	}
}

void Editor::DropAt(SelectionPosition position, const char *value, bool moving, bool rectangular) {
	DropAt(position, value, strlen(value), moving, rectangular);
}

/**
 * @return true if given position is inside the selection,
 */
bool Editor::PositionInSelection(Sci::Position pos) noexcept {
	pos = MovePositionOutsideChar(pos, sel.MainCaret() - pos);
	for (size_t r = 0; r < sel.Count(); r++) {
		if (sel.Range(r).Contains(pos))
			return true;
	}
	return false;
}

bool Editor::PointInSelection(Point pt) {
	const SelectionPosition pos = SPositionFromLocation(pt, false, true);
	const Point ptPos = LocationFromPosition(pos);
	for (size_t r = 0; r < sel.Count(); r++) {
		const SelectionRange &range = sel.Range(r);
		if (range.Contains(pos)) {
			bool hit = true;
			if (pos == range.Start()) {
				// see if just before selection
				if (pt.x < ptPos.x) {
					hit = false;
				}
			}
			if (pos == range.End()) {
				// see if just after selection
				if (pt.x > ptPos.x) {
					hit = false;
				}
			}
			if (hit)
				return true;
		}
	}
	return false;
}

bool Editor::PointInSelMargin(Point pt) const noexcept {
	// Really means: "Point in a margin"
	if (vs.fixedColumnWidth > 0) {	// There is a margin
		PRectangle rcSelMargin = GetClientRectangle();
		rcSelMargin.right = static_cast<XYPOSITION>(vs.textStart - vs.leftMarginWidth);
		rcSelMargin.left = static_cast<XYPOSITION>(vs.textStart - vs.fixedColumnWidth);
		const Point ptOrigin = GetVisibleOriginInMain();
		rcSelMargin.Move(0, -ptOrigin.y);
		return rcSelMargin.ContainsWholePixel(pt);
	} else {
		return false;
	}
}

Window::Cursor Editor::GetMarginCursor(Point pt) const noexcept {
	int x = 0;
	for (const auto &m : vs.ms) {
		if ((pt.x >= x) && (pt.x < x + m.width))
			return static_cast<Window::Cursor>(m.cursor);
		x += m.width;
	}
	return Window::Cursor::reverseArrow;
}

void Editor::TrimAndSetSelection(Sci::Position currentPos_, Sci::Position anchor_) {
	sel.TrimSelection(SelectionRange(currentPos_, anchor_));
	SetSelection(currentPos_, anchor_);
}

void Editor::LineSelection(Sci::Position lineCurrentPos_, Sci::Position lineAnchorPos_, bool wholeLine) {
	Sci::Position selCurrentPos;
	Sci::Position selAnchorPos;
	if (wholeLine) {
		const Sci::Line lineCurrent_ = pdoc->SciLineFromPosition(lineCurrentPos_);
		const Sci::Line lineAnchor_ = pdoc->SciLineFromPosition(lineAnchorPos_);
		if (lineAnchorPos_ < lineCurrentPos_) {
			selCurrentPos = pdoc->LineStart(lineCurrent_ + 1);
			selAnchorPos = pdoc->LineStart(lineAnchor_);
		} else if (lineAnchorPos_ > lineCurrentPos_) {
			selCurrentPos = pdoc->LineStart(lineCurrent_);
			selAnchorPos = pdoc->LineStart(lineAnchor_ + 1);
		} else { // Same line, select it
			selCurrentPos = pdoc->LineStart(lineAnchor_ + 1);
			selAnchorPos = pdoc->LineStart(lineAnchor_);
		}
	} else {
		if (lineAnchorPos_ < lineCurrentPos_) {
			selCurrentPos = StartEndDisplayLine(lineCurrentPos_, false) + 1;
			selCurrentPos = pdoc->MovePositionOutsideChar(selCurrentPos, 1);
			selAnchorPos = StartEndDisplayLine(lineAnchorPos_, true);
		} else if (lineAnchorPos_ > lineCurrentPos_) {
			selCurrentPos = StartEndDisplayLine(lineCurrentPos_, true);
			selAnchorPos = StartEndDisplayLine(lineAnchorPos_, false) + 1;
			selAnchorPos = pdoc->MovePositionOutsideChar(selAnchorPos, 1);
		} else { // Same line, select it
			selCurrentPos = StartEndDisplayLine(lineAnchorPos_, false) + 1;
			selCurrentPos = pdoc->MovePositionOutsideChar(selCurrentPos, 1);
			selAnchorPos = StartEndDisplayLine(lineAnchorPos_, true);
		}
	}
	TrimAndSetSelection(selCurrentPos, selAnchorPos);
}

void Editor::WordSelection(Sci::Position pos) {
	if (pos < wordSelectAnchorStartPos) {
		// Extend backward to the word containing pos.
		// Skip ExtendWordSelect if the line is empty or if pos is after the last character.
		// This ensures that a series of empty lines isn't counted as a single "word".
		if (!pdoc->IsLineEndPosition(pos))
			pos = pdoc->ExtendWordSelect(pdoc->MovePositionOutsideChar(pos + 1, 1), -1);
		TrimAndSetSelection(pos, wordSelectAnchorEndPos);
	} else if (pos > wordSelectAnchorEndPos) {
		// Extend forward to the word containing the character to the left of pos.
		// Skip ExtendWordSelect if the line is empty or if pos is the first position on the line.
		// This ensures that a series of empty lines isn't counted as a single "word".
		if (pos > pdoc->LineStart(pdoc->LineFromPosition(pos)))
			pos = pdoc->ExtendWordSelect(pdoc->MovePositionOutsideChar(pos - 1, -1), 1);
		TrimAndSetSelection(pos, wordSelectAnchorStartPos);
	} else {
		// Select only the anchored word
		if (pos >= originalAnchorPos)
			TrimAndSetSelection(wordSelectAnchorEndPos, wordSelectAnchorStartPos);
		else
			TrimAndSetSelection(wordSelectAnchorStartPos, wordSelectAnchorEndPos);
	}
}

void Editor::DwellEnd(bool mouseMoved) {
	if (mouseMoved)
		ticksToDwell = dwellDelay;
	else
		ticksToDwell = SC_TIME_FOREVER;
	if (dwelling && (dwellDelay < SC_TIME_FOREVER)) {
		dwelling = false;
		NotifyDwelling(ptMouseLast, dwelling);
	}
	FineTickerCancel(TickReason::dwell);
}

void Editor::MouseLeave() {
	SetHotSpotRange(nullptr);
	SetHoverIndicatorPosition(Sci::invalidPosition);
	if (!HaveMouseCapture()) {
		ptMouseLast = Point(-1, -1);
		DwellEnd(true);
	}
}

static constexpr bool AllowVirtualSpace(int virtualSpaceOptions, bool rectangular) noexcept {
	return (!rectangular && ((virtualSpaceOptions & SCVS_USERACCESSIBLE) != 0))
		|| (rectangular && ((virtualSpaceOptions & SCVS_RECTANGULARSELECTION) != 0));
}

void Editor::ButtonDownWithModifiers(Point pt, unsigned int curTime, int modifiers) {
	SetHoverIndicatorPoint(pt);
	//Platform::DebugPrintf("ButtonDown %d %d = %d modifiers=%d %d\n", curTime, lastClickTime, curTime - lastClickTime, modifiers, inDragDrop);
	ptMouseLast = pt;
	const bool ctrl = (modifiers & SCI_CTRL) != 0;
	const bool shift = (modifiers & SCI_SHIFT) != 0;
	const bool alt = (modifiers & SCI_ALT) != 0;
	SelectionPosition newPos = SPositionFromLocation(pt, false, false, AllowVirtualSpace(virtualSpaceOptions, alt));
	newPos = MovePositionOutsideChar(newPos, sel.MainCaret() - newPos.Position());
	SelectionPosition newCharPos = SPositionFromLocation(pt, false, true, false);
	newCharPos = MovePositionOutsideChar(newCharPos, -1);
	inDragDrop = DragDrop::none;
	sel.SetMoveExtends(false);

	if (NotifyMarginClick(pt, modifiers))
		return;

	NotifyIndicatorClick(true, newPos.Position(), modifiers);

	const bool inSelMargin = PointInSelMargin(pt);
	// In margin ctrl+(double)click should always select everything
	if (ctrl && inSelMargin) {
		SelectAll();
		lastClickTime = curTime;
		lastClick = pt;
		return;
	}
	if (shift && !inSelMargin) {
		SetSelection(newPos);
	}
	if ((curTime < (lastClickTime + Platform::DoubleClickTime())) && Close(pt, lastClick, doubleClickCloseThreshold)) {
		//Platform::DebugPrintf("Double click %d %d = %d\n", curTime, lastClickTime, curTime - lastClickTime);
		SetMouseCapture(true);
		FineTickerStart(TickReason::scroll, 100, 10);
		if (!ctrl || !multipleSelection || (selectionUnit != TextUnit::character && selectionUnit != TextUnit::word))
			SetEmptySelection(newPos.Position());
		bool doubleClick = false;
		if (inSelMargin) {
			// Inside margin selection type should be either subLine or wholeLine.
			if (selectionUnit == TextUnit::subLine) {
				// If it is subLine, we're inside a *double* click and word wrap is enabled,
				// so we switch to wholeLine in order to select whole line.
				selectionUnit = TextUnit::wholeLine;
			} else if (selectionUnit != TextUnit::subLine && selectionUnit != TextUnit::wholeLine) {
				// If it is neither, reset selection type to line selection.
				selectionUnit = (Wrapping() && (marginOptions & SC_MARGINOPTION_SUBLINESELECT)) ? TextUnit::subLine : TextUnit::wholeLine;
			}
		} else {
			if (selectionUnit == TextUnit::character) {
				selectionUnit = TextUnit::word;
				doubleClick = true;
			} else if (selectionUnit == TextUnit::word) {
				// Since we ended up here, we're inside a *triple* click, which should always select
				// whole line regardless of word wrap being enabled or not.
				selectionUnit = TextUnit::wholeLine;
			} else {
				selectionUnit = TextUnit::character;
				originalAnchorPos = sel.MainCaret();
			}
		}

		if (selectionUnit == TextUnit::word) {
			Sci::Position charPos = originalAnchorPos;
			if (sel.MainCaret() == originalAnchorPos) {
				charPos = PositionFromLocation(pt, false, true);
				charPos = MovePositionOutsideChar(charPos, -1);
			}

			Sci::Position startWord;
			Sci::Position endWord;
			if ((sel.MainCaret() >= originalAnchorPos) && !pdoc->IsLineEndPosition(charPos)) {
				startWord = pdoc->ExtendWordSelect(pdoc->MovePositionOutsideChar(charPos + 1, 1), -1);
				endWord = pdoc->ExtendWordSelect(charPos, 1);
			} else {
				// Selecting backwards, or anchor beyond last character on line. In these cases,
				// we select the word containing the character to the *left* of the anchor.
				if (charPos > pdoc->LineStart(pdoc->LineFromPosition(charPos))) {
					startWord = pdoc->ExtendWordSelect(charPos, -1);
					endWord = pdoc->ExtendWordSelect(startWord, 1);
				} else {
					// Anchor at start of line; select nothing to begin with.
					startWord = charPos;
					endWord = charPos;
				}
			}

			wordSelectAnchorStartPos = startWord;
			wordSelectAnchorEndPos = endWord;
			wordSelectInitialCaretPos = sel.MainCaret();
			WordSelection(wordSelectInitialCaretPos);
		} else if (selectionUnit == TextUnit::subLine || selectionUnit == TextUnit::wholeLine) {
			lineAnchorPos = newPos.Position();
			LineSelection(lineAnchorPos, lineAnchorPos, selectionUnit == TextUnit::wholeLine);
			//Platform::DebugPrintf("Triple click: %d - %d\n", anchor, currentPos);
		} else {
			SetEmptySelection(sel.MainCaret());
		}
		//Platform::DebugPrintf("Double click: %d - %d\n", anchor, currentPos);
		if (doubleClick) {
			NotifyDoubleClick(pt, modifiers);
			if (PositionIsHotspot(newCharPos.Position()))
				NotifyHotSpotDoubleClicked(newCharPos.Position(), modifiers);
		}
	} else {	// Single click
		if (inSelMargin) {
			if (sel.IsRectangular() || (sel.Count() > 1)) {
				InvalidateWholeSelection();
				sel.Clear();
			}
			sel.selType = Selection::SelTypes::stream;
			if (!shift) {
				// Single click in margin: select wholeLine or only subLine if word wrap is enabled
				lineAnchorPos = newPos.Position();
				selectionUnit = (Wrapping() && (marginOptions & SC_MARGINOPTION_SUBLINESELECT)) ? TextUnit::subLine : TextUnit::wholeLine;
				LineSelection(lineAnchorPos, lineAnchorPos, selectionUnit == TextUnit::wholeLine);
			} else {
				// Single shift+click in margin: select from line anchor to clicked line
				if (sel.MainAnchor() > sel.MainCaret())
					lineAnchorPos = sel.MainAnchor() - 1;
				else
					lineAnchorPos = sel.MainAnchor();
				// Reset selection type if there is an empty selection.
				// This ensures that we don't end up stuck in previous selection mode, which is no longer valid.
				// Otherwise, if there's a non empty selection, reset selection type only if it differs from selSubLine and selWholeLine.
				// This ensures that we continue selecting in the same selection mode.
				if (sel.Empty() || (selectionUnit != TextUnit::subLine && selectionUnit != TextUnit::wholeLine))
					selectionUnit = (Wrapping() && (marginOptions & SC_MARGINOPTION_SUBLINESELECT)) ? TextUnit::subLine : TextUnit::wholeLine;
				LineSelection(newPos.Position(), lineAnchorPos, selectionUnit == TextUnit::wholeLine);
			}

			SetDragPosition(SelectionPosition(Sci::invalidPosition));
			SetMouseCapture(true);
			FineTickerStart(TickReason::scroll, 100, 10);
		} else {
			if (PointIsHotspot(pt)) {
				NotifyHotSpotClicked(newCharPos.Position(), modifiers);
				hotSpotClickPos = newCharPos.Position();
			}
			if (!shift) {
				if (PointInSelection(pt) && !SelectionEmpty())
					inDragDrop = DragDrop::initial;
				else
					inDragDrop = DragDrop::none;
			}
			SetMouseCapture(true);
			FineTickerStart(TickReason::scroll, 100, 10);
			if (inDragDrop != DragDrop::initial) {
				SetDragPosition(SelectionPosition(Sci::invalidPosition));
				if (!shift) {
					if (ctrl && multipleSelection) {
						const SelectionRange range(newPos);
						sel.TentativeSelection(range);
						InvalidateSelection(range, true);
					} else {
						InvalidateSelection(SelectionRange(newPos), true);
						if (sel.Count() > 1)
							Redraw();
						if ((sel.Count() > 1) || (sel.selType != Selection::SelTypes::stream))
							sel.Clear();
						sel.selType = alt ? Selection::SelTypes::rectangle : Selection::SelTypes::stream;
						SetSelection(newPos, newPos);
					}
				}
				SelectionPosition anchorCurrent = newPos;
				if (shift)
					anchorCurrent = sel.IsRectangular() ?
					sel.Rectangular().anchor : sel.RangeMain().anchor;
				sel.selType = alt ? Selection::SelTypes::rectangle : Selection::SelTypes::stream;
				selectionUnit = TextUnit::character;
				originalAnchorPos = sel.MainCaret();
				sel.Rectangular() = SelectionRange(newPos, anchorCurrent);
				SetRectangularRange();
			}
		}
	}
	lastClickTime = curTime;
	lastClick = pt;
	lastXChosen = static_cast<int>(pt.x) + xOffset;
	ShowCaretAtCurrentPosition();
}

void Editor::RightButtonDownWithModifiers(Point pt, unsigned int, int modifiers) {
	if (NotifyMarginRightClick(pt, modifiers))
		return;
}

bool Editor::PositionIsHotspot(Sci::Position position) const noexcept {
	return vs.styles[pdoc->StyleIndexAt(position)].hotspot;
}

bool Editor::PointIsHotspot(Point pt) {
	const Sci::Position pos = PositionFromLocation(pt, true, true);
	if (pos == INVALID_POSITION)
		return false;
	return PositionIsHotspot(pos);
}

void Editor::SetHoverIndicatorPosition(Sci::Position position) noexcept {
	const Sci::Position hoverIndicatorPosPrev = hoverIndicatorPos;
	hoverIndicatorPos = INVALID_POSITION;
	if (!vs.indicatorsDynamic)
		return;
	if (position != INVALID_POSITION) {
		for (const auto *deco : pdoc->decorations->View()) {
			if (vs.indicators[deco->Indicator()].IsDynamic()) {
				if (pdoc->decorations->ValueAt(deco->Indicator(), position)) {
					hoverIndicatorPos = position;
				}
			}
		}
	}
	if (hoverIndicatorPosPrev != hoverIndicatorPos) {
		Redraw();
	}
}

void Editor::SetHoverIndicatorPoint(Point pt) {
	if (!vs.indicatorsDynamic) {
		SetHoverIndicatorPosition(INVALID_POSITION);
	} else {
		SetHoverIndicatorPosition(PositionFromLocation(pt, true, true));
	}
}

void Editor::SetHotSpotRange(const Point *pt) {
	if (pt) {
		const Sci::Position pos = PositionFromLocation(*pt, false, true);

		// If we don't limit this to word characters then the
		// range can encompass more than the run range and then
		// the underline will not be drawn properly.
		Range hsNew;
		hsNew.start = pdoc->ExtendStyleRange(pos, -1, hotspotSingleLine);
		hsNew.end = pdoc->ExtendStyleRange(pos, 1, hotspotSingleLine);

		// Only invalidate the range if the hotspot range has changed...
		if (!(hsNew == hotspot)) {
			if (hotspot.Valid()) {
				InvalidateRange(hotspot.start, hotspot.end);
			}
			hotspot = hsNew;
			InvalidateRange(hotspot.start, hotspot.end);
		}
	} else {
		if (hotspot.Valid()) {
			InvalidateRange(hotspot.start, hotspot.end);
		}
		hotspot = Range(Sci::invalidPosition);
	}
}

Range Editor::GetHotSpotRange() const noexcept {
	return hotspot;
}

void Editor::ButtonMoveWithModifiers(Point pt, unsigned int, int modifiers) {
	if (ptMouseLast != pt) {
		DwellEnd(true);
	}

	SelectionPosition movePos = SPositionFromLocation(pt, false, false,
		AllowVirtualSpace(virtualSpaceOptions, sel.IsRectangular()));
	movePos = MovePositionOutsideChar(movePos, sel.MainCaret() - movePos.Position());

	if (inDragDrop == DragDrop::initial) {
		if (DragThreshold(ptMouseLast, pt)) {
			SetMouseCapture(false);
			FineTickerCancel(TickReason::scroll);
			SetDragPosition(movePos);
			CopySelectionRange(&drag);
			StartDrag();
		}
		return;
	}

	ptMouseLast = pt;
	PRectangle rcClient = GetClientRectangle();
	const Point ptOrigin = GetVisibleOriginInMain();
	rcClient.Move(0, -ptOrigin.y);
	if ((dwellDelay < SC_TIME_FOREVER) && rcClient.Contains(pt)) {
		FineTickerStart(TickReason::dwell, dwellDelay, dwellDelay / 10);
	}
	//Platform::DebugPrintf("Move %.0f %.0f\n", pt.x, pt.y);
	if (HaveMouseCapture()) {

		// Slow down autoscrolling/selection
		autoScrollTimer.ticksToWait -= timer.tickSize;
		if (autoScrollTimer.ticksToWait > 0)
			return;
		autoScrollTimer.ticksToWait = autoScrollDelay;

		// Adjust selection
		if (posDrag.IsValid()) {
			SetDragPosition(movePos);
		} else {
			if (selectionUnit == TextUnit::character) {
				if (sel.selType == Selection::SelTypes::stream && (modifiers & SCI_ALT) && mouseSelectionRectangularSwitch) {
					sel.selType = Selection::SelTypes::rectangle;
				}
				if (sel.IsRectangular()) {
					sel.Rectangular() = SelectionRange(movePos, sel.Rectangular().anchor);
					SetSelection(movePos, sel.RangeMain().anchor);
				} else if (sel.Count() > 1) {
					InvalidateSelection(sel.RangeMain(), false);
					const SelectionRange range(movePos, sel.RangeMain().anchor);
					sel.TentativeSelection(range);
					InvalidateSelection(range, true);
				} else {
					SetSelection(movePos, sel.RangeMain().anchor);
				}
			} else if (selectionUnit == TextUnit::word) {
				// Continue selecting by word
				if (movePos.Position() == wordSelectInitialCaretPos) {  // Didn't move
					// No need to do anything. Previously this case was lumped
					// in with "Moved forward", but that can be harmful in this
					// case: a handler for the NotifyDoubleClick re-adjusts
					// the selection for a fancier definition of "word" (for
					// example, in Perl it is useful to include the leading
					// '$', '%' or '@' on variables for word selection). In this
					// the ButtonMove() called via TickFor() for auto-scrolling
					// could result in the fancier word selection adjustment
					// being unmade.
				} else {
					wordSelectInitialCaretPos = -1;
					WordSelection(movePos.Position());
				}
			} else {
				// Continue selecting by line
				LineSelection(movePos.Position(), lineAnchorPos, selectionUnit == TextUnit::wholeLine);
			}
		}

		// Autoscroll
		const Sci::Line lineMove = DisplayFromPosition(movePos.Position());
		if (pt.y >= rcClient.bottom) {
			ScrollTo(lineMove - LinesOnScreen() + 1);
			Redraw();
		} else if (pt.y < rcClient.top) {
			ScrollTo(lineMove);
			Redraw();
		}
		EnsureCaretVisible(false, false, true);

		if (hotspot.Valid() && !PointIsHotspot(pt))
			SetHotSpotRange(nullptr);

		if (hotSpotClickPos != INVALID_POSITION && PositionFromLocation(pt, true, true) != hotSpotClickPos) {
			if (inDragDrop == DragDrop::none) {
				DisplayCursor(Window::Cursor::text);
			}
			hotSpotClickPos = INVALID_POSITION;
		}

	} else {
		if (vs.fixedColumnWidth > 0) {	// There is a margin
			if (PointInSelMargin(pt)) {
				DisplayCursor(GetMarginCursor(pt));
				SetHotSpotRange(nullptr);
				SetHoverIndicatorPosition(Sci::invalidPosition);
				return; 	// No need to test for selection
			}
		}
		// Display regular (drag) cursor over selection
		if (PointInSelection(pt) && !SelectionEmpty()) {
			DisplayCursor(Window::Cursor::arrow);
			SetHoverIndicatorPosition(Sci::invalidPosition);
		} else {
			SetHoverIndicatorPoint(pt);
			if (PointIsHotspot(pt)) {
				DisplayCursor(Window::Cursor::hand);
				SetHotSpotRange(&pt);
			} else {
				if (hoverIndicatorPos != Sci::invalidPosition)
					DisplayCursor(Window::Cursor::hand);
				else
					DisplayCursor(Window::Cursor::text);
				SetHotSpotRange(nullptr);
			}
		}
	}
}

void Editor::ButtonUpWithModifiers(Point pt, unsigned int curTime, int modifiers) {
	//Platform::DebugPrintf("ButtonUp %d %d\n", HaveMouseCapture(), inDragDrop);
	SelectionPosition newPos = SPositionFromLocation(pt, false, false,
		AllowVirtualSpace(virtualSpaceOptions, sel.IsRectangular()));
	if (hoverIndicatorPos != INVALID_POSITION)
		InvalidateRange(newPos.Position(), newPos.Position() + 1);
	newPos = MovePositionOutsideChar(newPos, sel.MainCaret() - newPos.Position());
	if (inDragDrop == DragDrop::initial) {
		inDragDrop = DragDrop::none;
		SetEmptySelection(newPos);
		selectionUnit = TextUnit::character;
		originalAnchorPos = sel.MainCaret();
	}
	if (hotSpotClickPos != INVALID_POSITION && PointIsHotspot(pt)) {
		hotSpotClickPos = INVALID_POSITION;
		SelectionPosition newCharPos = SPositionFromLocation(pt, false, true, false);
		newCharPos = MovePositionOutsideChar(newCharPos, -1);
		NotifyHotSpotReleaseClick(newCharPos.Position(), modifiers & SCI_CTRL);
	}
	if (HaveMouseCapture()) {
		if (PointInSelMargin(pt)) {
			DisplayCursor(GetMarginCursor(pt));
		} else {
			DisplayCursor(Window::Cursor::text);
			SetHotSpotRange(nullptr);
		}
		ptMouseLast = pt;
		SetMouseCapture(false);
		FineTickerCancel(TickReason::scroll);
		NotifyIndicatorClick(false, newPos.Position(), modifiers);
		if (inDragDrop == DragDrop::dragging) {
			const SelectionPosition selStart = SelectionStart();
			const SelectionPosition selEnd = SelectionEnd();
			if (selStart < selEnd) {
				if (drag.Length()) {
					const Sci::Position length = drag.Length();
					if (modifiers & SCI_CTRL) {
						const Sci::Position lengthInserted = pdoc->InsertString(
							newPos.Position(), drag.Data(), length);
						if (lengthInserted > 0) {
							SetSelection(newPos.Position(), newPos.Position() + lengthInserted);
						}
					} else if (newPos < selStart) {
						pdoc->DeleteChars(selStart.Position(), drag.Length());
						const Sci::Position lengthInserted = pdoc->InsertString(
							newPos.Position(), drag.Data(), length);
						if (lengthInserted > 0) {
							SetSelection(newPos.Position(), newPos.Position() + lengthInserted);
						}
					} else if (newPos > selEnd) {
						pdoc->DeleteChars(selStart.Position(), drag.Length());
						newPos.Add(-static_cast<Sci::Position>(drag.Length()));
						const Sci::Position lengthInserted = pdoc->InsertString(
							newPos.Position(), drag.Data(), length);
						if (lengthInserted > 0) {
							SetSelection(newPos.Position(), newPos.Position() + lengthInserted);
						}
					} else {
						SetEmptySelection(newPos.Position());
					}
					drag.Clear();
				}
				selectionUnit = TextUnit::character;
			}
		} else {
			if (selectionUnit == TextUnit::character) {
				if (sel.Count() > 1) {
					sel.RangeMain() =
						SelectionRange(newPos, sel.Range(sel.Count() - 1).anchor);
					InvalidateWholeSelection();
				} else {
					SetSelection(newPos, sel.RangeMain().anchor);
				}
			}
			sel.CommitTentative();
		}
		SetRectangularRange();
		lastClickTime = curTime;
		lastClick = pt;
		lastXChosen = static_cast<int>(pt.x) + xOffset;
		if (sel.selType == Selection::SelTypes::stream) {
			SetLastXChosen();
		}
		inDragDrop = DragDrop::none;
		EnsureCaretVisible(false);
	}
}

bool Editor::Idle() {
	NotifyUpdateUI();

	bool needWrap = Wrapping() && wrapPending.NeedsWrap();

	if (needWrap) {
		// Wrap lines during idle.
		WrapLines(WrapScope::wsIdle);
		// No more wrapping
		needWrap = wrapPending.NeedsWrap();
	} else if (needIdleStyling) {
		IdleStyling();
	}

	// Add more idle things to do here, but make sure idleDone is
	// set correctly before the function returns. returning
	// false will stop calling this idle function until SetIdle() is
	// called again.

	const bool idleDone = !needWrap && !needIdleStyling; // && thatDone && theOtherThingDone...

	return !idleDone;
}

void Editor::TickFor(TickReason reason) {
	switch (reason) {
	case TickReason::caret:
		caret.on = !caret.on;
		if (caret.active) {
			InvalidateCaret();
		}
		break;
	case TickReason::scroll:
		// Auto scroll
		ButtonMoveWithModifiers(ptMouseLast, 0, 0);
		break;
	case TickReason::widen:
		SetScrollBars();
		FineTickerCancel(TickReason::widen);
		break;
	case TickReason::dwell:
		if ((!HaveMouseCapture()) &&
			(ptMouseLast.y >= 0)) {
			dwelling = true;
			NotifyDwelling(ptMouseLast, dwelling);
		}
		FineTickerCancel(TickReason::dwell);
		break;
	default:
		// tickPlatform handled by subclass
		break;
	}
}

// FineTickerStart is be overridden by subclasses that support fine ticking so
// this method should never be called.
bool Editor::FineTickerRunning(TickReason) noexcept {
	assert(false);
	return false;
}

// FineTickerStart is be overridden by subclasses that support fine ticking so
// this method should never be called.
void Editor::FineTickerStart(TickReason, int, int) noexcept {
	assert(false);
}

// FineTickerCancel is be overridden by subclasses that support fine ticking so
// this method should never be called.
void Editor::FineTickerCancel(TickReason) noexcept {
	assert(false);
}

void Editor::SetFocusState(bool focusState) {
	const bool changing = hasFocus != focusState;
	hasFocus = focusState;
	if (changing) {
		Redraw();
	}
	NotifyFocus(hasFocus);
	if (!hasFocus) {
		CancelModes();
	}
	ShowCaretAtCurrentPosition();
}

Sci::Position Editor::PositionAfterArea(PRectangle rcArea) const noexcept {
	// The start of the document line after the display line after the area
	// This often means that the line after a modification is restyled which helps
	// detect multiline comment additions and heals single line comments
	const Sci::Line lineAfter = TopLineOfMain() + static_cast<Sci::Line>(rcArea.bottom - 1) / vs.lineHeight + 1;
	if (lineAfter < pcs->LinesDisplayed())
		return pdoc->LineStart(pcs->DocFromDisplay(lineAfter) + 1);
	else
		return pdoc->Length();
}

// Style to a position within the view. If this causes a change at end of last line then
// affects later lines so style all the viewed text.
void Editor::StyleToPositionInView(Sci::Position pos) {
	Sci::Position endWindow = PositionAfterArea(GetClientDrawingRectangle());
	pos = std::min(pos, endWindow);
	const int styleAtEnd = pdoc->StyleIndexAt(pos - 1);
	pdoc->EnsureStyledTo(pos);
	if ((endWindow > pos) && (styleAtEnd != pdoc->StyleIndexAt(pos - 1))) {
		// Style at end of line changed so is multi-line change like starting a comment
		// so require rest of window to be styled.
		DiscardOverdraw();	// Prepared bitmaps may be invalid
		// DiscardOverdraw may have truncated client drawing area so recalculate endWindow
		endWindow = PositionAfterArea(GetClientDrawingRectangle());
		pdoc->EnsureStyledTo(endWindow);
	}
}

Sci::Position Editor::PositionAfterMaxStyling(Sci::Position posMax, bool scrolling) const noexcept {
	if (SynchronousStylingToVisible()) {
		// Both states do not limit styling
		return posMax;
	}

	// Try to keep time taken by styling reasonable so interaction remains smooth.
	// When scrolling, allow less time to ensure responsive
	const double secondsAllowed = scrolling ? 0.005 : 0.02;

	Sci::Line lineLast = pdoc->SciLineFromPosition(pdoc->GetEndStyled());
	const Sci::Position actionsInAllowedTime = pdoc->durationStyleOneUnit.ActionsInAllowedTime(secondsAllowed);
	lineLast = pdoc->LineFromPositionAfter(lineLast, actionsInAllowedTime);

	const Sci::Line stylingMaxLine = std::min(lineLast, pdoc->LinesTotal());
	return std::min(pdoc->LineStart(stylingMaxLine), posMax);
}

void Editor::StartIdleStyling(bool truncatedLastStyling) noexcept {
	if ((idleStyling == SC_IDLESTYLING_ALL) || (idleStyling == SC_IDLESTYLING_AFTERVISIBLE)) {
		if (pdoc->GetEndStyled() < pdoc->Length()) {
			// Style remainder of document in idle time
			needIdleStyling = true;
		}
	} else if (truncatedLastStyling) {
		needIdleStyling = true;
	}

	if (needIdleStyling) {
		SetIdle(true);
	}
}

// Style for an area but bound the amount of styling to remain responsive
void Editor::StyleAreaBounded(PRectangle rcArea, bool scrolling) {
	const Sci::Position posAfterArea = PositionAfterArea(rcArea);
	const Sci::Position posAfterMax = PositionAfterMaxStyling(posAfterArea, scrolling);
	if (posAfterMax < posAfterArea) {
		// Idle styling may be performed before current visible area
		// Style a bit now then style further in idle time
		pdoc->StyleToAdjustingLineDuration(posAfterMax);
	} else {
		// Can style all wanted now.
		StyleToPositionInView(posAfterArea);
	}
	StartIdleStyling(posAfterMax < posAfterArea);
}

void Editor::IdleStyling() {
	const Sci::Position posAfterArea = PositionAfterArea(GetClientRectangle());
	const Sci::Position endGoal = (idleStyling >= SC_IDLESTYLING_AFTERVISIBLE) ?
		pdoc->Length() : posAfterArea;
	const Sci::Position posAfterMax = PositionAfterMaxStyling(endGoal, false);
	pdoc->StyleToAdjustingLineDuration(posAfterMax);
	if (pdoc->GetEndStyled() >= endGoal) {
		needIdleStyling = false;
	}
}

void Editor::IdleWork() {
	// Style the line after the modification as this allows modifications that change just the
	// line of the modification to heal instead of propagating to the rest of the window.
	if (FlagSet(workNeeded.items, WorkItems::style)) {
		StyleToPositionInView(pdoc->LineStart(pdoc->LineFromPosition(workNeeded.upTo) + 2));
	}
	NotifyUpdateUI();
	workNeeded.Reset();
}

void Editor::QueueIdleWork(WorkItems items, Sci::Position upTo) noexcept {
	workNeeded.Need(items, upTo);
}

int Editor::SupportsFeature(int feature) const {
	AutoSurface surface(this);
	return surface->Supports(feature);
}

bool Editor::PaintContains(PRectangle rc) const noexcept {
	if (rc.Empty()) {
		return true;
	} else {
		return rcPaint.Contains(rc);
	}
}

bool Editor::PaintContainsMargin() const noexcept {
	if (wMargin.GetID()) {
		// With separate margin view, paint of text view
		// never contains margin.
		return false;
	}
	PRectangle rcSelMargin = GetClientRectangle();
	rcSelMargin.right = static_cast<XYPOSITION>(vs.textStart);
	return PaintContains(rcSelMargin);
}

void Editor::CheckForChangeOutsidePaint(Range r) noexcept {
	if (paintState == PaintState::painting && !paintingAllText) {
		//Platform::DebugPrintf("Checking range in paint %d-%d\n", r.start, r.end);
		if (!r.Valid())
			return;

		PRectangle rcRange = RectangleFromRange(r, 0);
		const PRectangle rcText = GetTextRectangle();
		rcRange.top = std::max(rcRange.top, rcText.top);
		rcRange.bottom = std::min(rcRange.bottom, rcText.bottom);

		if (!PaintContains(rcRange)) {
			AbandonPaint();
			paintAbandonedByStyling = true;
		}
	}
}

void Editor::SetBraceHighlight(Sci::Position pos0, Sci::Position pos1, int matchStyle) noexcept {
	if ((pos0 != braces[0]) || (pos1 != braces[1]) || (matchStyle != bracesMatchStyle)) {
		if ((braces[0] != pos0) || (matchStyle != bracesMatchStyle)) {
			CheckForChangeOutsidePaint(Range(braces[0]));
			CheckForChangeOutsidePaint(Range(pos0));
			braces[0] = pos0;
		}
		if ((braces[1] != pos1) || (matchStyle != bracesMatchStyle)) {
			CheckForChangeOutsidePaint(Range(braces[1]));
			CheckForChangeOutsidePaint(Range(pos1));
			braces[1] = pos1;
		}
		bracesMatchStyle = matchStyle;
		if (paintState == PaintState::notPainting) {
			Redraw();
		}
	}
}

void Editor::SetAnnotationHeights(Sci::Line start, Sci::Line end) {
	if (vs.annotationVisible) {
		RefreshStyleData();
		bool changedHeight = false;
		for (Sci::Line line = start; line < end && line < pdoc->LinesTotal(); line++) {
			int linesWrapped = 1;
			if (Wrapping()) {
				AutoSurface surface(this);
				LineLayout *ll = view.RetrieveLineLayout(line, *this);
				if (surface && ll) {
					view.LayoutLine(*this, surface, vs, ll, wrapWidth);
					linesWrapped = ll->lines;
				}
			}
			if (pcs->SetHeight(line, pdoc->AnnotationLines(line) + linesWrapped))
				changedHeight = true;
		}
		if (changedHeight) {
			Redraw();
		}
	}
}

void Editor::SetDocPointer(Document *document) {
	//Platform::DebugPrintf("** %p setdoc to %p\n", pdoc, document);
	pdoc->RemoveWatcher(this, nullptr);
	pdoc->Release();
	if (document == nullptr) {
		pdoc = new Document(SC_DOCUMENTOPTION_DEFAULT);
	} else {
		pdoc = document;
	}
	pdoc->AddRef();
	pcs = ContractionStateCreate(pdoc->IsLarge());

	// Ensure all positions within document
	sel.Clear();
	targetRange = SelectionSegment();

	braces[0] = Sci::invalidPosition;
	braces[1] = Sci::invalidPosition;

	vs.ReleaseAllExtendedStyles();

	SetRepresentations();

	// Reset the contraction state to fully shown.
	pcs->Clear();
	pcs->InsertLines(0, pdoc->LinesTotal() - 1);
	SetAnnotationHeights(0, pdoc->LinesTotal());
	view.llc.Deallocate();
	NeedWrapping();

	hotspot = Range(Sci::invalidPosition);
	hoverIndicatorPos = Sci::invalidPosition;

	view.ClearAllTabstops();

	pdoc->AddWatcher(this, nullptr);
	SetScrollBars();
	Redraw();
}

void Editor::SetAnnotationVisible(int visible) {
	if (vs.annotationVisible != visible) {
		const bool changedFromOrToHidden = ((vs.annotationVisible != 0) != (visible != 0));
		vs.annotationVisible = visible;
		if (changedFromOrToHidden) {
			const int dir = vs.annotationVisible ? 1 : -1;
			for (Sci::Line line = 0; line < pdoc->LinesTotal(); line++) {
				const int annotationLines = pdoc->AnnotationLines(line);
				if (annotationLines > 0) {
					pcs->SetHeight(line, pcs->GetHeight(line) + annotationLines * dir);
				}
			}
			SetScrollBars();
		}
		Redraw();
	}
}

void Editor::SetEOLAnnotationVisible(int visible) noexcept {
	if (vs.eolAnnotationVisible != visible) {
		vs.eolAnnotationVisible = visible;
		Redraw();
	}
}

/**
 * Recursively expand a fold, making lines visible except where they have an unexpanded parent.
 */
Sci::Line Editor::ExpandLine(Sci::Line line) {
	const Sci::Line lineMaxSubord = pdoc->GetLastChild(line);
	line++;
	while (line <= lineMaxSubord) {
		pcs->SetVisible(line, line, true);
		const int level = pdoc->GetLevel(line);
		if (level & SC_FOLDLEVELHEADERFLAG) {
			if (pcs->GetExpanded(line)) {
				line = ExpandLine(line);
			} else {
				line = pdoc->GetLastChild(line);
			}
		}
		line++;
	}
	return lineMaxSubord;
}

void Editor::SetFoldExpanded(Sci::Line lineDoc, bool expanded) {
	if (pcs->SetExpanded(lineDoc, expanded)) {
		RedrawSelMargin();
	}
}

void Editor::FoldLine(Sci::Line line, int action) {
	if (line >= 0) {
		if (action == SC_FOLDACTION_TOGGLE) {
			if ((pdoc->GetLevel(line) & SC_FOLDLEVELHEADERFLAG) == 0) {
				line = pdoc->GetFoldParent(line);
				if (line < 0)
					return;
			}
			action = (pcs->GetExpanded(line)) ? SC_FOLDACTION_CONTRACT : SC_FOLDACTION_EXPAND;
		}

		if (action == SC_FOLDACTION_CONTRACT) {
			const Sci::Line lineMaxSubord = pdoc->GetLastChild(line);
			if (lineMaxSubord > line) {
				pcs->SetExpanded(line, false);
				pcs->SetVisible(line + 1, lineMaxSubord, false);

				const Sci::Line lineCurrent =
					pdoc->SciLineFromPosition(sel.MainCaret());
				if (lineCurrent > line && lineCurrent <= lineMaxSubord) {
					// This does not re-expand the fold
					EnsureCaretVisible();
				}
			}

		} else {
			if (!(pcs->GetVisible(line))) {
				EnsureLineVisible(line, false);
				GoToLine(line);
			}
			pcs->SetExpanded(line, true);
			ExpandLine(line);
		}

		SetScrollBars();
		Redraw();
	}
}

void Editor::FoldExpand(Sci::Line line, int action, int level) {
	bool expanding = action == SC_FOLDACTION_EXPAND;
	if (action == SC_FOLDACTION_TOGGLE) {
		expanding = !pcs->GetExpanded(line);
	}
	// Ensure child lines lexed and fold information extracted before
	// flipping the state.
	pdoc->GetLastChild(line, LevelNumber(level));
	SetFoldExpanded(line, expanding);
	if (expanding && (pcs->HiddenLines() == 0))
		// Nothing to do
		return;
	const Sci::Line lineMaxSubord = pdoc->GetLastChild(line, LevelNumber(level));
	line++;
	pcs->SetVisible(line, lineMaxSubord, expanding);
	while (line <= lineMaxSubord) {
		const int levelLine = pdoc->GetLevel(line);
		if (levelLine & SC_FOLDLEVELHEADERFLAG) {
			SetFoldExpanded(line, expanding);
		}
		line++;
	}
	SetScrollBars();
	Redraw();
}

Sci::Line Editor::ContractedFoldNext(Sci::Line lineStart) const noexcept {
	for (Sci::Line line = lineStart; line < pdoc->LinesTotal();) {
		if (!pcs->GetExpanded(line) && (pdoc->GetLevel(line) & SC_FOLDLEVELHEADERFLAG))
			return line;
		line = pcs->ContractedNext(line + 1);
		if (line < 0)
			return -1;
	}

	return -1;
}

/**
 * Recurse up from this line to find any folds that prevent this line from being visible
 * and unfold them all.
 */
void Editor::EnsureLineVisible(Sci::Line lineDoc, bool enforcePolicy) {
	// In case in need of wrapping to ensure DisplayFromDoc works.
	if (lineDoc >= wrapPending.start) {
		if (WrapLines(WrapScope::wsAll)) {
			Redraw();
		}
	}

	if (!pcs->GetVisible(lineDoc)) {
		// Back up to find a non-blank line
		Sci::Line lookLine = lineDoc;
		int lookLineLevel = pdoc->GetLevel(lookLine);
		while ((lookLine > 0) && (lookLineLevel & SC_FOLDLEVELWHITEFLAG)) {
			lookLineLevel = pdoc->GetLevel(--lookLine);
		}
		Sci::Line lineParent = pdoc->GetFoldParent(lookLine);
		if (lineParent < 0) {
			// Backed up to a top level line, so try to find parent of initial line
			lineParent = pdoc->GetFoldParent(lineDoc);
		}
		if (lineParent >= 0) {
			if (lineDoc != lineParent)
				EnsureLineVisible(lineParent, enforcePolicy);
			if (!pcs->GetExpanded(lineParent)) {
				pcs->SetExpanded(lineParent, true);
				ExpandLine(lineParent);
			}
		}
		SetScrollBars();
		Redraw();
	}
	if (enforcePolicy) {
		const Sci::Line lineDisplay = pcs->DisplayFromDoc(lineDoc);
		if (visiblePolicy.policy & VISIBLE_SLOP) {
			if ((topLine > lineDisplay) || ((visiblePolicy.policy & VISIBLE_STRICT) && (topLine + visiblePolicy.slop > lineDisplay))) {
				SetTopLine(std::clamp<Sci::Line>(lineDisplay - visiblePolicy.slop, 0, MaxScrollPos()));
				SetVerticalScrollPos();
				Redraw();
			} else if ((lineDisplay > topLine + LinesOnScreen() - 1) ||
				((visiblePolicy.policy & VISIBLE_STRICT) && (lineDisplay > topLine + LinesOnScreen() - 1 - visiblePolicy.slop))) {
				SetTopLine(std::clamp<Sci::Line>(lineDisplay - LinesOnScreen() + 1 + visiblePolicy.slop, 0, MaxScrollPos()));
				SetVerticalScrollPos();
				Redraw();
			}
		} else {
			if ((topLine > lineDisplay) || (lineDisplay > topLine + LinesOnScreen() - 1) || (visiblePolicy.policy & VISIBLE_STRICT)) {
				SetTopLine(std::clamp<Sci::Line>(lineDisplay - LinesOnScreen() / 2 + 1, 0, MaxScrollPos()));
				SetVerticalScrollPos();
				Redraw();
			}
		}
	}
}

void Editor::FoldAll(int action) {
	pdoc->EnsureStyledTo(pdoc->Length());
	const Sci::Line maxLine = pdoc->LinesTotal();
	bool expanding = action == SC_FOLDACTION_EXPAND;
	if (action == SC_FOLDACTION_TOGGLE) {
		// Discover current state
		for (int lineSeek = 0; lineSeek < maxLine; lineSeek++) {
			if (pdoc->GetLevel(lineSeek) & SC_FOLDLEVELHEADERFLAG) {
				expanding = !pcs->GetExpanded(lineSeek);
				break;
			}
		}
	}
	if (expanding) {
		pcs->SetVisible(0, maxLine - 1, true);
		for (int line = 0; line < maxLine; line++) {
			const int levelLine = pdoc->GetLevel(line);
			if (levelLine & SC_FOLDLEVELHEADERFLAG) {
				SetFoldExpanded(line, true);
			}
		}
	} else {
		for (Sci::Line line = 0; line < maxLine; line++) {
			const int level = pdoc->GetLevel(line);
			if ((level & SC_FOLDLEVELHEADERFLAG) &&
				(SC_FOLDLEVELBASE == LevelNumber(level))) {
				SetFoldExpanded(line, false);
				const Sci::Line lineMaxSubord = pdoc->GetLastChild(line, -1);
				if (lineMaxSubord > line) {
					pcs->SetVisible(line + 1, lineMaxSubord, false);
				}
			}
		}
	}
	SetScrollBars();
	Redraw();
}

void Editor::FoldChanged(Sci::Line line, int levelNow, int levelPrev) {
	if (levelNow & SC_FOLDLEVELHEADERFLAG) {
		if (!(levelPrev & SC_FOLDLEVELHEADERFLAG)) {
			// Adding a fold point.
			if (pcs->SetExpanded(line, true)) {
				RedrawSelMargin();
			}
			FoldExpand(line, SC_FOLDACTION_EXPAND, levelPrev);
		}
	} else if (levelPrev & SC_FOLDLEVELHEADERFLAG) {
		const Sci::Line prevLine = line - 1;
		const int prevLineLevel = pdoc->GetLevel(prevLine);

		// Combining two blocks where the first block is collapsed (e.g. by deleting the line(s) which separate(s) the two blocks)
		if ((LevelNumber(prevLineLevel) == LevelNumber(levelNow)) && !pcs->GetVisible(prevLine))
			FoldLine(pdoc->GetFoldParent(prevLine), SC_FOLDACTION_EXPAND);

		if (!pcs->GetExpanded(line)) {
			// Removing the fold from one that has been contracted so should expand
			// otherwise lines are left invisible with no way to make them visible
			if (pcs->SetExpanded(line, true)) {
				RedrawSelMargin();
			}
			// Combining two blocks where the second one is collapsed (e.g. by adding characters in the line which separates the two blocks)
			FoldExpand(line, SC_FOLDACTION_EXPAND, levelPrev);
		}
	}
	if (!(levelNow & SC_FOLDLEVELWHITEFLAG) &&
		(LevelNumber(levelPrev) > LevelNumber(levelNow))) {
		if (pcs->HiddenLines()) {
			// See if should still be hidden
			const Sci::Line parentLine = pdoc->GetFoldParent(line);
			if ((parentLine < 0) || (pcs->GetExpanded(parentLine) && pcs->GetVisible(parentLine))) {
				pcs->SetVisible(line, line, true);
				SetScrollBars();
				Redraw();
			}
		}
	}

	// Combining two blocks where the first one is collapsed (e.g. by adding characters in the line which separates the two blocks)
	if (!(levelNow & SC_FOLDLEVELWHITEFLAG) && (LevelNumber(levelPrev) < LevelNumber(levelNow))) {
		if (pcs->HiddenLines()) {
			const Sci::Line parentLine = pdoc->GetFoldParent(line);
			if (!pcs->GetExpanded(parentLine) && pcs->GetVisible(line))
				FoldLine(parentLine, SC_FOLDACTION_EXPAND);
		}
	}
}

void Editor::NeedShown(Sci::Position pos, Sci::Position len) {
	if (foldAutomatic & SC_AUTOMATICFOLD_SHOW) {
		const Sci::Line lineStart = pdoc->SciLineFromPosition(pos);
		const Sci::Line lineEnd = pdoc->SciLineFromPosition(pos + len);
		for (Sci::Line line = lineStart; line <= lineEnd; line++) {
			EnsureLineVisible(line, false);
		}
	} else {
		NotifyNeedShown(pos, len);
	}
}

Sci::Position Editor::GetTag(char *tagValue, int tagNumber) {
	const char *text = nullptr;
	Sci::Position length = 0;
	if ((tagNumber >= 1) && (tagNumber <= 9)) {
		char name[3] = "\\?";
		name[1] = static_cast<char>(tagNumber + '0');
		length = 2;
		text = pdoc->SubstituteByPosition(name, &length);
	}
	if (tagValue) {
		if (text)
			memcpy(tagValue, text, length + 1);
		else
			*tagValue = '\0';
	}
	return length;
}

Sci::Position Editor::ReplaceTarget(bool replacePatterns, const char *text, Sci::Position length) {
	UndoGroup ug(pdoc);
	if (length == -1)
		length = strlen(text);
	if (replacePatterns) {
		text = pdoc->SubstituteByPosition(text, &length);
		if (!text) {
			return 0;
		}
	}

	// Remove the text inside the range
	if (targetRange.Length() > 0) {
		pdoc->DeleteChars(targetRange.start.Position(), targetRange.Length());
	}
	targetRange.end = targetRange.start;

	// Realize virtual space of target start
	const Sci::Position startAfterSpaceInsertion = RealizeVirtualSpace(targetRange.start.Position(), targetRange.start.VirtualSpace());
	targetRange.start.SetPosition(startAfterSpaceInsertion);
	targetRange.end = targetRange.start;

	// Insert the new text
	const Sci::Position lengthInserted = pdoc->InsertString(targetRange.start.Position(), text, length);
	targetRange.end.SetPosition(targetRange.start.Position() + lengthInserted);
	return length;
}

bool Editor::IsUnicodeMode() const noexcept {
	return pdoc && (SC_CP_UTF8 == pdoc->dbcsCodePage);
}

int Editor::CodePage() const noexcept {
	if (pdoc)
		return pdoc->dbcsCodePage;
	else
		return 0;
}

Sci::Line Editor::WrapCount(Sci::Line line) {
	AutoSurface surface(this);
	LineLayout *ll = view.RetrieveLineLayout(line, *this);

	if (surface && ll) {
		view.LayoutLine(*this, surface, vs, ll, wrapWidth);
		return ll->lines;
	} else {
		return 1;
	}
}

void Editor::AddStyledText(const char *buffer, Sci::Position appendLength) {
	// The buffer consists of alternating character bytes and style bytes
	const Sci::Position textLength = appendLength / 2;
	std::string text(textLength, '\0');
	Sci::Position i;
	for (i = 0; i < textLength; i++) {
		text[i] = buffer[i * 2];
	}
	const Sci::Position lengthInserted = pdoc->InsertString(CurrentPosition(), text.c_str(), textLength);
	for (i = 0; i < textLength; i++) {
		text[i] = buffer[i * 2 + 1];
	}
	pdoc->StartStyling(CurrentPosition());
	pdoc->SetStyles(textLength, reinterpret_cast<const unsigned char*>(text.c_str()));
	SetEmptySelection(sel.MainCaret() + lengthInserted);
}

bool Editor::ValidMargin(uptr_t wParam) const noexcept {
	return wParam < vs.ms.size();
}

void Editor::StyleSetMessage(unsigned int iMessage, uptr_t wParam, sptr_t lParam) {
	vs.EnsureStyle(wParam);
	switch (iMessage) {
	case SCI_STYLESETFORE:
		vs.styles[wParam].fore = ColourAlpha::FromRGB(static_cast<unsigned int>(lParam));
		break;
	case SCI_STYLESETBACK:
		vs.styles[wParam].back = ColourAlpha::FromRGB(static_cast<unsigned int>(lParam));
		break;
	case SCI_STYLESETBOLD:
		vs.fontsValid = false;
		vs.styles[wParam].weight = lParam != 0 ? SC_WEIGHT_BOLD : SC_WEIGHT_NORMAL;
		break;
	case SCI_STYLESETWEIGHT:
		vs.fontsValid = false;
		vs.styles[wParam].weight = static_cast<int>(lParam);
		break;
	case SCI_STYLESETITALIC:
		vs.fontsValid = false;
		vs.styles[wParam].italic = lParam != 0;
		break;
	case SCI_STYLESETEOLFILLED:
		vs.styles[wParam].eolFilled = lParam != 0;
		break;
	case SCI_STYLESETSIZE:
		vs.fontsValid = false;
		vs.styles[wParam].size = static_cast<int>(lParam * SC_FONT_SIZE_MULTIPLIER);
		break;
	case SCI_STYLESETSIZEFRACTIONAL:
		vs.fontsValid = false;
		vs.styles[wParam].size = static_cast<int>(lParam);
		break;
	case SCI_STYLESETFONT:
		if (lParam != 0) {
			vs.SetStyleFontName(static_cast<int>(wParam), CharPtrFromSPtr(lParam));
		}
		break;
	case SCI_STYLESETUNDERLINE:
		vs.styles[wParam].underline = lParam != 0;
		break;
		// Added strike style, 2011-12-20
	case SCI_STYLESETSTRIKE:
		vs.styles[wParam].strike = lParam != 0;
		break;
	case SCI_STYLESETCASE:
		vs.styles[wParam].caseForce = static_cast<Style::CaseForce>(lParam);
		break;
	case SCI_STYLESETCHARACTERSET:
		vs.fontsValid = false;
		vs.styles[wParam].characterSet = static_cast<int>(lParam);
		break;
	case SCI_STYLESETVISIBLE:
		vs.styles[wParam].visible = lParam != 0;
		break;
	case SCI_STYLESETCHANGEABLE:
		vs.styles[wParam].changeable = lParam != 0;
		break;
	case SCI_STYLESETHOTSPOT:
		vs.styles[wParam].hotspot = lParam != 0;
		break;
	}
	InvalidateStyleRedraw();
}

sptr_t Editor::StyleGetMessage(unsigned int iMessage, uptr_t wParam, sptr_t lParam) {
	vs.EnsureStyle(wParam);
	switch (iMessage) {
	case SCI_STYLEGETFORE:
		return vs.styles[wParam].fore.OpaqueRGB();
	case SCI_STYLEGETBACK:
		return vs.styles[wParam].back.OpaqueRGB();
	case SCI_STYLEGETBOLD:
		return vs.styles[wParam].weight > SC_WEIGHT_NORMAL;
	case SCI_STYLEGETWEIGHT:
		return vs.styles[wParam].weight;
	case SCI_STYLEGETITALIC:
		return vs.styles[wParam].italic ? 1 : 0;
	case SCI_STYLEGETEOLFILLED:
		return vs.styles[wParam].eolFilled ? 1 : 0;
	case SCI_STYLEGETSIZE:
		return vs.styles[wParam].size / SC_FONT_SIZE_MULTIPLIER;
	case SCI_STYLEGETSIZEFRACTIONAL:
		return vs.styles[wParam].size;
	case SCI_STYLEGETFONT:
		return StringResult(lParam, vs.styles[wParam].fontName);
	case SCI_STYLEGETUNDERLINE:
		return vs.styles[wParam].underline ? 1 : 0;
	case SCI_STYLEGETCASE:
		return static_cast<int>(vs.styles[wParam].caseForce);
	case SCI_STYLEGETCHARACTERSET:
		return vs.styles[wParam].characterSet;
	case SCI_STYLEGETVISIBLE:
		return vs.styles[wParam].visible ? 1 : 0;
	case SCI_STYLEGETCHANGEABLE:
		return vs.styles[wParam].changeable ? 1 : 0;
	case SCI_STYLEGETHOTSPOT:
		return vs.styles[wParam].hotspot ? 1 : 0;
	}
	return 0;
}

void Editor::SetSelectionNMessage(unsigned int iMessage, uptr_t wParam, sptr_t lParam) noexcept {
	if (wParam >= sel.Count()) {
		return;
	}
	InvalidateRange(sel.Range(wParam).Start().Position(), sel.Range(wParam).End().Position());

	switch (iMessage) {
	case SCI_SETSELECTIONNCARET:
		sel.Range(wParam).caret.SetPosition(lParam);
		break;

	case SCI_SETSELECTIONNANCHOR:
		sel.Range(wParam).anchor.SetPosition(lParam);
		break;

	case SCI_SETSELECTIONNCARETVIRTUALSPACE:
		sel.Range(wParam).caret.SetVirtualSpace(lParam);
		break;

	case SCI_SETSELECTIONNANCHORVIRTUALSPACE:
		sel.Range(wParam).anchor.SetVirtualSpace(lParam);
		break;

	case SCI_SETSELECTIONNSTART:
		sel.Range(wParam).anchor.SetPosition(lParam);
		break;

	case SCI_SETSELECTIONNEND:
		sel.Range(wParam).caret.SetPosition(lParam);
		break;
	}

	InvalidateRange(sel.Range(wParam).Start().Position(), sel.Range(wParam).End().Position());
	ContainerNeedsUpdate(SC_UPDATE_SELECTION);
}

sptr_t Editor::StringResult(sptr_t lParam, const char *val) noexcept {
	const size_t len = val ? strlen(val) : 0;
	if (lParam) {
		char *ptr = CharPtrFromSPtr(lParam);
		if (val)
			memcpy(ptr, val, len + 1);
		else
			*ptr = 0;
	}
	return len;	// Not including NUL
}

sptr_t Editor::BytesResult(sptr_t lParam, const unsigned char *val, size_t len) noexcept {
	// No NUL termination: len is number of valid/displayed bytes
	if ((lParam) && (len > 0)) {
		char *ptr = CharPtrFromSPtr(lParam);
		if (val)
			memcpy(ptr, val, len);
		else
			*ptr = 0;
	}
	return val ? len : 0;
}

sptr_t Editor::WndProc(unsigned int iMessage, uptr_t wParam, sptr_t lParam) {
	//Platform::DebugPrintf("S start wnd proc %u %d %d\n", iMessage, wParam, lParam);

	// Optional macro recording hook
	if (recordingMacro)
		NotifyMacroRecord(iMessage, wParam, lParam);

	switch (iMessage) {

	case SCI_GETTEXT: {
			if (lParam == 0)
				return pdoc->Length() + 1;
			if (wParam == 0)
				return 0;
			char *ptr = CharPtrFromSPtr(lParam);
			const Sci_Position len = std::min<Sci_Position>(wParam - 1, pdoc->Length());
			pdoc->GetCharRange(ptr, 0, len);
			ptr[len] = '\0';
			return len;
		}

	case SCI_SETTEXT: {
			if (lParam == 0)
				return 0;
			UndoGroup ug(pdoc);
			pdoc->DeleteChars(0, pdoc->Length());
			SetEmptySelection(0);
			const char *text = CharPtrFromSPtr(lParam);
			pdoc->InsertString(0, text, strlen(text));
			return 1;
		}

	case SCI_GETTEXTLENGTH:
		return pdoc->Length();

	case SCI_CUT:
		Cut(wParam != 0);
		SetLastXChosen();
		break;

	case SCI_COPY:
		Copy(wParam != 0);
		break;

	case SCI_COPYALLOWLINE:
		CopyAllowLine();
		break;

	case SCI_VERTICALCENTRECARET:
		VerticalCentreCaret();
		break;

	case SCI_MOVESELECTEDLINESUP:
		MoveSelectedLinesUp();
		break;

	case SCI_MOVESELECTEDLINESDOWN:
		MoveSelectedLinesDown();
		break;

	case SCI_COPYRANGE:
		CopyRangeToClipboard(wParam, lParam);
		break;

	case SCI_COPYTEXT:
		CopyText(wParam, CharPtrFromSPtr(lParam));
		break;

	case SCI_PASTE:
		Paste(wParam != 0);
		if ((caretSticky == SC_CARETSTICKY_OFF) || (caretSticky == SC_CARETSTICKY_WHITESPACE)) {
			SetLastXChosen();
		}
		EnsureCaretVisible();
		break;

	case SCI_CLEAR:
		Clear();
		SetLastXChosen();
		EnsureCaretVisible();
		break;

	case SCI_UNDO:
		Undo();
		SetLastXChosen();
		break;

	case SCI_CANUNDO:
		return (pdoc->CanUndo() && !pdoc->IsReadOnly()) ? 1 : 0;

	case SCI_EMPTYUNDOBUFFER:
		pdoc->DeleteUndoHistory();
		return 0;

	case SCI_GETFIRSTVISIBLELINE:
		return topLine;

	case SCI_SETFIRSTVISIBLELINE:
		ScrollTo(wParam);
		break;

	case SCI_GETLINE: {	// Risk of overwriting the end of the buffer
			const Sci::Position lineStart =
				pdoc->LineStart(wParam);
			const Sci::Position lineEnd =
				pdoc->LineStart(wParam + 1);
			// not NULL terminated
			const Sci::Position len = lineEnd - lineStart;
			if (lParam == 0) {
				return len;
			}
			char *ptr = CharPtrFromSPtr(lParam);
			pdoc->GetCharRange(ptr, lineStart, len);
			ptr[len] = '\0'; //! overwriting
			return len;
		}

	case SCI_GETLINECOUNT:
		if (pdoc->LinesTotal() == 0)
			return 1;
		else
			return pdoc->LinesTotal();

	case SCI_ALLOCATELINES:
		pdoc->AllocateLines(wParam);
		break;

	case SCI_GETMODIFY:
		return !pdoc->IsSavePoint();

	case SCI_SETSEL: {
			Sci::Position nStart = wParam;
			Sci::Position nEnd = lParam;
			if (nEnd < 0)
				nEnd = pdoc->Length();
			if (nStart < 0)
				nStart = nEnd; 	// Remove selection
			InvalidateSelection(SelectionRange(nStart, nEnd));
			sel.Clear();
			sel.selType = Selection::SelTypes::stream;
			SetSelection(nEnd, nStart);
			EnsureCaretVisible();
		}
		break;

	case SCI_GETSELTEXT: {
			SelectionText selectedText;
			CopySelectionRange(&selectedText);
			if (lParam == 0) {
				return selectedText.LengthWithTerminator();
			} else {
				char *ptr = CharPtrFromSPtr(lParam);
				const size_t iChar = selectedText.Length();
				memcpy(ptr, selectedText.Data(), iChar);
				ptr[iChar] = '\0';
				return iChar;
			}
		}

	case SCI_LINEFROMPOSITION:
		if (static_cast<Sci::Position>(wParam) < 0)
			return 0;
		return pdoc->LineFromPosition(wParam);

	case SCI_POSITIONFROMLINE:
		if (static_cast<Sci::Position>(wParam) < 0)
			wParam = pdoc->LineFromPosition(SelectionStart().Position());
		if (wParam == 0)
			return 0; 	// Even if there is no text, there is a first line that starts at 0
		if (static_cast<Sci::Line>(wParam) > pdoc->LinesTotal())
			return -1;
		//if (wParam > pdoc->LineFromPosition(pdoc->Length()))	// Useful test, anyway...
		//	return -1;
		return pdoc->LineStart(wParam);

		// Replacement of the old Scintilla interpretation of EM_LINELENGTH
	case SCI_LINELENGTH:
		if ((static_cast<Sci::Position>(wParam) < 0) ||
		        (static_cast<Sci::Position>(wParam) > pdoc->LineFromPosition(pdoc->Length())))
			return 0;
		return pdoc->LineStart(wParam + 1) - pdoc->LineStart(wParam);

	case SCI_REPLACESEL: {
			if (lParam == 0)
				return 0;
			UndoGroup ug(pdoc);
			ClearSelection();
			const char *replacement = CharPtrFromSPtr(lParam);
			const Sci::Position lengthInserted = pdoc->InsertString(
				sel.MainCaret(), replacement, strlen(replacement));
			SetEmptySelection(sel.MainCaret() + lengthInserted);
			SetLastXChosen();
			EnsureCaretVisible();
		}
		break;

	case SCI_SETTARGETSTART:
		targetRange.start.SetPosition(static_cast<Sci::Position>(wParam));
		break;

	case SCI_GETTARGETSTART:
		return targetRange.start.Position();

	case SCI_SETTARGETSTARTVIRTUALSPACE:
		targetRange.start.SetVirtualSpace(static_cast<Sci::Position>(wParam));
		break;

	case SCI_GETTARGETSTARTVIRTUALSPACE:
		return targetRange.start.VirtualSpace();

	case SCI_SETTARGETEND:
		targetRange.end.SetPosition(static_cast<Sci::Position>(wParam));
		break;

	case SCI_GETTARGETEND:
		return targetRange.end.Position();

	case SCI_SETTARGETENDVIRTUALSPACE:
		targetRange.end.SetVirtualSpace(static_cast<Sci::Position>(wParam));
		break;

	case SCI_GETTARGETENDVIRTUALSPACE:
		return targetRange.end.VirtualSpace();

	case SCI_SETTARGETRANGE:
		targetRange.start.SetPosition(static_cast<Sci::Position>(wParam));
		targetRange.end.SetPosition(lParam);
		break;

	case SCI_TARGETWHOLEDOCUMENT:
		targetRange.start.SetPosition(0);
		targetRange.end.SetPosition(pdoc->Length());
		break;

	case SCI_TARGETFROMSELECTION:
		targetRange.start = sel.RangeMain().Start();
		targetRange.end = sel.RangeMain().End();
		break;

	case SCI_GETTARGETTEXT: {
			std::string text = RangeText(targetRange.start.Position(), targetRange.end.Position());
			return BytesResult(lParam, reinterpret_cast<const unsigned char *>(text.c_str()), text.length());
		}

	case SCI_REPLACETARGET:
		PLATFORM_ASSERT(lParam);
		return ReplaceTarget(false, CharPtrFromSPtr(lParam), wParam);

	case SCI_REPLACETARGETRE:
		PLATFORM_ASSERT(lParam);
		return ReplaceTarget(true, CharPtrFromSPtr(lParam), wParam);

	case SCI_SEARCHINTARGET:
		PLATFORM_ASSERT(lParam);
		return SearchInTarget(CharPtrFromSPtr(lParam), wParam);

	case SCI_SETSEARCHFLAGS:
		searchFlags = static_cast<int>(wParam);
		break;

	case SCI_GETSEARCHFLAGS:
		return searchFlags;

	case SCI_GETTAG:
		return GetTag(CharPtrFromSPtr(lParam), static_cast<int>(wParam));

	case SCI_POSITIONBEFORE:
		return pdoc->MovePositionOutsideChar(wParam - 1, -1, true);

	case SCI_POSITIONAFTER:
		return pdoc->MovePositionOutsideChar(wParam + 1, 1, true);

	case SCI_POSITIONRELATIVE:
		return std::clamp<Sci::Position>(pdoc->GetRelativePosition(
			wParam, lParam),
			0, pdoc->Length());

	case SCI_POSITIONRELATIVECODEUNITS:
		return std::clamp<Sci::Position>(pdoc->GetRelativePositionUTF16(
			wParam, lParam),
			0, pdoc->Length());

	case SCI_LINESCROLL:
		ScrollTo(topLine + lParam);
		HorizontalScrollTo(xOffset + static_cast<int>(wParam) * static_cast<int>(vs.spaceWidth));
		return 1;

	case SCI_SETXOFFSET:
		xOffset = static_cast<int>(wParam);
		ContainerNeedsUpdate(SC_UPDATE_H_SCROLL);
		SetHorizontalScrollPos();
		Redraw();
		break;

	case SCI_GETXOFFSET:
		return xOffset;

	case SCI_CHOOSECARETX:
		SetLastXChosen();
		break;

	case SCI_SCROLLCARET:
		EnsureCaretVisible();
		break;

	case SCI_SETREADONLY:
		pdoc->SetReadOnly(wParam != 0);
		return 1;

	case SCI_GETREADONLY:
		return pdoc->IsReadOnly();

	case SCI_CANPASTE:
		return CanPaste();

	case SCI_POINTXFROMPOSITION:
		if (lParam < 0) {
			return 0;
		} else {
			const Point pt = LocationFromPosition(lParam);
			// Convert to view-relative
			return static_cast<int>(pt.x) - vs.textStart + vs.fixedColumnWidth;
		}

	case SCI_POINTYFROMPOSITION:
		if (lParam < 0) {
			return 0;
		} else {
			const Point pt = LocationFromPosition(lParam);
			return static_cast<int>(pt.y);
		}

	case SCI_FINDTEXT:
		return FindText(wParam, lParam);

	case SCI_GETTEXTRANGE: {
			if (lParam == 0)
				return 0;
			Sci_TextRange *tr = static_cast<Sci_TextRange *>(PtrFromSPtr(lParam));
			Sci::Position cpMax = tr->chrg.cpMax;
			if (cpMax == -1)
				cpMax = pdoc->Length();
			PLATFORM_ASSERT(cpMax <= pdoc->Length());
			Sci::Position len = cpMax - tr->chrg.cpMin; 	// No -1 as cpMin and cpMax are referring to inter character positions
			pdoc->GetCharRange(tr->lpstrText, tr->chrg.cpMin, len);
			// Spec says copied text is terminated with a NUL
			tr->lpstrText[len] = '\0';
			return len; 	// Not including NUL
		}

	case SCI_HIDESELECTION:
		view.hideSelection = wParam != 0;
		Redraw();
		break;

	case SCI_FORMATRANGE:
		return FormatRange(wParam != 0, static_cast<Sci_RangeToFormat *>(PtrFromSPtr(lParam)));

	case SCI_GETMARGINLEFT:
		return vs.leftMarginWidth;

	case SCI_GETMARGINRIGHT:
		return vs.rightMarginWidth;

	case SCI_SETMARGINLEFT:
		lastXChosen += static_cast<int>(lParam) - vs.leftMarginWidth;
		vs.leftMarginWidth = static_cast<int>(lParam);
		InvalidateStyleRedraw();
		break;

	case SCI_SETMARGINRIGHT:
		vs.rightMarginWidth = static_cast<int>(lParam);
		InvalidateStyleRedraw();
		break;

		// Control specific messages

	case SCI_ADDTEXT: {
			if (lParam == 0)
				return 0;
			const Sci::Position lengthInserted = pdoc->InsertString(
				CurrentPosition(), CharPtrFromSPtr(lParam), wParam);
			SetEmptySelection(sel.MainCaret() + lengthInserted);
			return 0;
		}

	case SCI_ADDSTYLEDTEXT:
		if (lParam)
			AddStyledText(CharPtrFromSPtr(lParam), wParam);
		return 0;

	case SCI_INSERTTEXT: {
			if (lParam == 0)
				return 0;
			Sci::Position insertPos = wParam;
			if (insertPos == -1)
				insertPos = CurrentPosition();
			Sci::Position newCurrent = CurrentPosition();
			const char *sz = CharPtrFromSPtr(lParam);
			const Sci::Position lengthInserted = pdoc->InsertString(insertPos, sz, strlen(sz));
			if (newCurrent > insertPos)
				newCurrent += lengthInserted;
			SetEmptySelection(newCurrent);
			return 0;
		}

	case SCI_CHANGEINSERTION:
		PLATFORM_ASSERT(lParam);
		pdoc->ChangeInsertion(CharPtrFromSPtr(lParam), wParam);
		return 0;

	case SCI_APPENDTEXT:
		pdoc->InsertString(pdoc->Length(),
			CharPtrFromSPtr(lParam), wParam);
		return 0;

	case SCI_CLEARALL:
		ClearAll();
		return 0;

	case SCI_DELETERANGE:
		pdoc->DeleteChars(wParam, lParam);
		return 0;

	case SCI_CLEARDOCUMENTSTYLE:
		ClearDocumentStyle();
		return 0;

	case SCI_SETUNDOCOLLECTION:
		pdoc->SetUndoCollection(wParam != 0);
		return 0;

	case SCI_GETUNDOCOLLECTION:
		return pdoc->IsCollectingUndo();

	case SCI_BEGINUNDOACTION:
		pdoc->BeginUndoAction();
		return 0;

	case SCI_ENDUNDOACTION:
		pdoc->EndUndoAction();
		return 0;

	case SCI_GETCARETPERIOD:
		return caret.period;

	case SCI_SETCARETPERIOD:
		CaretSetPeriod(static_cast<int>(wParam));
		break;

	case SCI_GETWORDCHARS:
		return pdoc->GetCharsOfClass(CharacterClass::word, UCharPtrFromSPtr(lParam));

	case SCI_SETWORDCHARS: {
			pdoc->SetDefaultCharClasses(false);
			if (lParam == 0)
				return 0;
			pdoc->SetCharClasses(ConstUCharPtrFromSPtr(lParam), CharacterClass::word);
		}
		break;

	case SCI_SETCHARCLASSESEX:
		pdoc->SetCharClassesEx(ConstUCharPtrFromSPtr(lParam), static_cast<int>(wParam));
		break;

	case SCI_GETWHITESPACECHARS:
		return pdoc->GetCharsOfClass(CharacterClass::space, UCharPtrFromSPtr(lParam));

	case SCI_SETWHITESPACECHARS: {
			if (lParam == 0)
				return 0;
			pdoc->SetCharClasses(ConstUCharPtrFromSPtr(lParam), CharacterClass::space);
		}
		break;

	case SCI_GETPUNCTUATIONCHARS:
		return pdoc->GetCharsOfClass(CharacterClass::punctuation, UCharPtrFromSPtr(lParam));

	case SCI_SETPUNCTUATIONCHARS: {
			if (lParam == 0)
				return 0;
			pdoc->SetCharClasses(ConstUCharPtrFromSPtr(lParam), CharacterClass::punctuation);
		}
		break;

	case SCI_SETCHARSDEFAULT:
		pdoc->SetDefaultCharClasses(true);
		break;

#if 0
	case SCI_SETCHARACTERCATEGORYOPTIMIZATION:
		pdoc->SetCharacterCategoryOptimization(static_cast<int>(wParam));
		break;

	case SCI_GETCHARACTERCATEGORYOPTIMIZATION:
		return pdoc->CharacterCategoryOptimization();
#endif

	case SCI_GETLENGTH:
		return pdoc->Length();

	case SCI_ALLOCATE:
		pdoc->Allocate(wParam);
		break;

	case SCI_GETCHARAT:
		return pdoc->UCharAt(wParam);

	case SCI_GETCHARACTERANDWIDTH:
		return pdoc->GetCharacterAndWidth(wParam, reinterpret_cast<Sci_Position *>(lParam));

	case SCI_ISAUTOCOMPLETIONWORDCHARACTER:
		return pdoc->IsAutoCompletionWordCharacter(static_cast<unsigned int>(wParam));

	case SCI_SETCURRENTPOS:
		if (sel.IsRectangular()) {
			sel.Rectangular().caret.SetPosition(wParam);
			SetRectangularRange();
			Redraw();
		} else {
			SetSelection(wParam, sel.MainAnchor());
		}
		break;

	case SCI_GETCURRENTPOS:
		return sel.IsRectangular() ? sel.Rectangular().caret.Position() : sel.MainCaret();

	case SCI_SETANCHOR:
		if (sel.IsRectangular()) {
			sel.Rectangular().anchor.SetPosition(wParam);
			SetRectangularRange();
			Redraw();
		} else {
			SetSelection(sel.MainCaret(), wParam);
		}
		break;

	case SCI_GETANCHOR:
		return sel.IsRectangular() ? sel.Rectangular().anchor.Position() : sel.MainAnchor();

	case SCI_SETSELECTIONSTART:
		SetSelection(std::max(sel.MainCaret(), static_cast<Sci::Position>(wParam)), wParam);
		break;

	case SCI_GETSELECTIONSTART:
		return sel.LimitsForRectangularElseMain().start.Position();

	case SCI_SETSELECTIONEND:
		SetSelection(wParam, std::min(sel.MainAnchor(), static_cast<Sci::Position>(wParam)));
		break;

	case SCI_GETSELECTIONEND:
		return sel.LimitsForRectangularElseMain().end.Position();

	case SCI_SETEMPTYSELECTION:
		SetEmptySelection(wParam);
		break;

	case SCI_SETPRINTMAGNIFICATION:
		view.printParameters.magnification = std::clamp(static_cast<int>(wParam), SC_MIN_ZOOM_LEVEL, SC_MAX_ZOOM_LEVEL);
		break;

	case SCI_GETPRINTMAGNIFICATION:
		return view.printParameters.magnification;

	case SCI_SETPRINTCOLOURMODE:
		view.printParameters.colourMode = static_cast<int>(wParam);
		break;

	case SCI_GETPRINTCOLOURMODE:
		return view.printParameters.colourMode;

	case SCI_SETPRINTWRAPMODE:
		view.printParameters.wrapState = (wParam == SC_WRAP_WORD) ? WrapMode::word : WrapMode::none;
		break;

	case SCI_GETPRINTWRAPMODE:
		return static_cast<sptr_t>(view.printParameters.wrapState);

	case SCI_GETSTYLEAT:
		if (static_cast<Sci::Position>(wParam) >= pdoc->Length())
			return 0;
		else
			return pdoc->StyleAt(wParam);

	case SCI_REDO:
		Redo();
		break;

	case SCI_SELECTALL:
		SelectAll();
		break;

	case SCI_SETSAVEPOINT:
		pdoc->SetSavePoint();
		break;

	case SCI_GETSTYLEDTEXT: {
			if (lParam == 0)
				return 0;
			Sci_TextRange *tr = static_cast<Sci_TextRange *>(PtrFromSPtr(lParam));
			Sci::Position iPlace = 0;
			for (Sci::Position iChar = tr->chrg.cpMin; iChar < tr->chrg.cpMax; iChar++) {
				tr->lpstrText[iPlace++] = pdoc->CharAt(iChar);
				tr->lpstrText[iPlace++] = pdoc->StyleAt(iChar);
			}
			tr->lpstrText[iPlace] = '\0';
			tr->lpstrText[iPlace + 1] = '\0';
			return iPlace;
		}

	case SCI_CANREDO:
		return (pdoc->CanRedo() && !pdoc->IsReadOnly()) ? 1 : 0;

	case SCI_MARKERLINEFROMHANDLE:
		return pdoc->LineFromHandle(static_cast<int>(wParam));

	case SCI_MARKERDELETEHANDLE:
		pdoc->DeleteMarkFromHandle(static_cast<int>(wParam));
		break;

	case SCI_MARKERHANDLEFROMLINE:
		return pdoc->MarkerHandleFromLine(wParam, static_cast<int>(lParam));

	case SCI_MARKERNUMBERFROMLINE:
		return pdoc->MarkerNumberFromLine(wParam, static_cast<int>(lParam));

	case SCI_GETVIEWWS:
		return static_cast<sptr_t>(vs.viewWhitespace);

	case SCI_SETVIEWWS:
		vs.viewWhitespace = static_cast<WhiteSpace>(wParam);
		Redraw();
		break;

	case SCI_GETTABDRAWMODE:
		return static_cast<sptr_t>(vs.tabDrawMode);

	case SCI_SETTABDRAWMODE:
		vs.tabDrawMode = static_cast<TabDrawMode>(wParam);
		Redraw();
		break;

	case SCI_GETWHITESPACESIZE:
		return vs.whitespaceSize;

	case SCI_SETWHITESPACESIZE:
		vs.whitespaceSize = static_cast<int>(wParam);
		Redraw();
		break;

	case SCI_POSITIONFROMPOINT:
		return PositionFromLocation(Point::FromInts(static_cast<int>(wParam) - vs.ExternalMarginWidth(), static_cast<int>(lParam)),
					    false, false);

	case SCI_POSITIONFROMPOINTCLOSE:
		return PositionFromLocation(Point::FromInts(static_cast<int>(wParam) - vs.ExternalMarginWidth(), static_cast<int>(lParam)),
					    true, false);

	case SCI_CHARPOSITIONFROMPOINT:
		return PositionFromLocation(Point::FromInts(static_cast<int>(wParam) - vs.ExternalMarginWidth(), static_cast<int>(lParam)),
					    false, true);

	case SCI_CHARPOSITIONFROMPOINTCLOSE:
		return PositionFromLocation(Point::FromInts(static_cast<int>(wParam) - vs.ExternalMarginWidth(), static_cast<int>(lParam)),
					    true, true);

	case SCI_GOTOLINE:
		GoToLine(wParam);
		break;

	case SCI_GOTOPOS:
		SetEmptySelection(wParam);
		EnsureCaretVisible();
		break;

	case SCI_GETCURLINE: {
			const Sci::Line lineCurrentPos = pdoc->SciLineFromPosition(sel.MainCaret());
			const Sci::Position lineStart = pdoc->LineStart(lineCurrentPos);
			const Sci::Position lineEnd = pdoc->LineStart(lineCurrentPos + 1);
			if (lParam == 0) {
				return 1 + lineEnd - lineStart;
			}
			PLATFORM_ASSERT(wParam > 0);
			char *ptr = CharPtrFromSPtr(lParam);
			const Sci::Position len = std::min<uptr_t>(lineEnd - lineStart, wParam - 1);
			pdoc->GetCharRange(ptr, lineStart, len);
			ptr[len] = '\0';
			return sel.MainCaret() - lineStart;
		}

	case SCI_GETENDSTYLED:
		return pdoc->GetEndStyled();

	case SCI_GETEOLMODE:
		return pdoc->eolMode;

	case SCI_SETEOLMODE:
		pdoc->eolMode = static_cast<int>(wParam);
		break;

	case SCI_SETLINEENDTYPESALLOWED:
		if (pdoc->SetLineEndTypesAllowed(static_cast<int>(wParam))) {
			pcs->Clear();
			pcs->InsertLines(0, pdoc->LinesTotal() - 1);
			SetAnnotationHeights(0, pdoc->LinesTotal());
			InvalidateStyleRedraw();
		}
		break;

	case SCI_GETLINEENDTYPESALLOWED:
		return pdoc->GetLineEndTypesAllowed();

	case SCI_GETLINEENDTYPESACTIVE:
		return pdoc->GetLineEndTypesActive();

	case SCI_STARTSTYLING:
		pdoc->StartStyling(wParam);
		break;

	case SCI_SETSTYLING:
		if (static_cast<Sci::Position>(wParam) < 0)
			errorStatus = SC_STATUS_FAILURE;
		else
			pdoc->SetStyleFor(wParam, static_cast<unsigned char>(lParam));
		break;

	case SCI_SETSTYLINGEX:             // Specify a complete styling buffer
		if (lParam == 0)
			return 0;
		pdoc->SetStyles(wParam, UCharPtrFromSPtr(lParam));
		break;

	case SCI_SETBUFFEREDDRAW:
		view.bufferedDraw = wParam != 0;
		break;

	case SCI_GETBUFFEREDDRAW:
		return view.bufferedDraw;

	case SCI_GETPHASESDRAW:
		return static_cast<sptr_t>(view.phasesDraw);

	case SCI_SETPHASESDRAW:
		if (view.SetPhasesDraw(static_cast<int>(wParam)))
			InvalidateStyleRedraw();
		break;

	case SCI_SETFONTQUALITY:
		vs.extraFontFlag &= ~SC_EFF_QUALITY_MASK;
		vs.extraFontFlag |= (wParam & SC_EFF_QUALITY_MASK);
		vs.fontsValid = false;
		InvalidateStyleRedraw();
		break;

	case SCI_GETFONTQUALITY:
		return (vs.extraFontFlag & SC_EFF_QUALITY_MASK);

	case SCI_SETTABWIDTH:
		if (wParam > 0) {
			pdoc->tabInChars = static_cast<int>(wParam);
			if (pdoc->indentInChars == 0)
				pdoc->actualIndentInChars = pdoc->tabInChars;
		}
		InvalidateStyleRedraw();
		break;

	case SCI_GETTABWIDTH:
		return pdoc->tabInChars;

	case SCI_SETTABMINIMUMWIDTH:
		SetAppearance(view.tabWidthMinimumPixels, static_cast<int>(wParam));
		break;

	case SCI_GETTABMINIMUMWIDTH:
		return view.tabWidthMinimumPixels;

	case SCI_CLEARTABSTOPS:
		if (view.ClearTabstops(wParam)) {
			const DocModification mh(SC_MOD_CHANGETABSTOPS, 0, 0, 0, nullptr, wParam);
			NotifyModified(pdoc, mh, nullptr);
		}
		break;

	case SCI_ADDTABSTOP:
		if (view.AddTabstop(wParam, static_cast<int>(lParam))) {
			const DocModification mh(SC_MOD_CHANGETABSTOPS, 0, 0, 0, nullptr, wParam);
			NotifyModified(pdoc, mh, nullptr);
		}
		break;

	case SCI_GETNEXTTABSTOP:
		return view.GetNextTabstop(wParam, static_cast<int>(lParam));

	case SCI_SETINDENT:
		pdoc->indentInChars = static_cast<int>(wParam);
		if (pdoc->indentInChars != 0)
			pdoc->actualIndentInChars = pdoc->indentInChars;
		else
			pdoc->actualIndentInChars = pdoc->tabInChars;
		InvalidateStyleRedraw();
		break;

	case SCI_GETINDENT:
		return pdoc->indentInChars;

	case SCI_SETUSETABS:
		pdoc->useTabs = wParam != 0;
		InvalidateStyleRedraw();
		break;

	case SCI_GETUSETABS:
		return pdoc->useTabs;

	case SCI_SETLINEINDENTATION:
		pdoc->SetLineIndentation(wParam, lParam);
		break;

	case SCI_GETLINEINDENTATION:
		return pdoc->GetLineIndentation(wParam);

	case SCI_GETLINEINDENTPOSITION:
		return pdoc->GetLineIndentPosition(wParam);

	case SCI_SETTABINDENTS:
		pdoc->tabIndents = wParam != 0;
		break;

	case SCI_GETTABINDENTS:
		return pdoc->tabIndents;

	case SCI_SETBACKSPACEUNINDENTS:
		pdoc->backspaceUnindents = wParam != 0;
		break;

	case SCI_GETBACKSPACEUNINDENTS:
		return pdoc->backspaceUnindents;

	case SCI_SETMOUSEDWELLTIME:
		dwellDelay = static_cast<int>(wParam);
		ticksToDwell = dwellDelay;
		break;

	case SCI_GETMOUSEDWELLTIME:
		return dwellDelay;

	case SCI_WORDSTARTPOSITION:
		return pdoc->ExtendWordSelect(wParam, -1, lParam != 0);

	case SCI_WORDENDPOSITION:
		return pdoc->ExtendWordSelect(wParam, 1, lParam != 0);

	case SCI_ISRANGEWORD:
		return pdoc->IsWordAt(wParam, lParam);

	case SCI_SETIDLESTYLING:
		idleStyling = static_cast<int>(wParam);
		break;

	case SCI_GETIDLESTYLING:
		return idleStyling;

	case SCI_SETWRAPMODE:
		if (vs.SetWrapState(static_cast<int>(wParam))) {
			xOffset = 0;
			ContainerNeedsUpdate(SC_UPDATE_H_SCROLL);
			InvalidateStyleRedraw();
			ReconfigureScrollBars();
		}
		break;

	case SCI_GETWRAPMODE:
		return static_cast<sptr_t>(vs.wrap.state);

	case SCI_SETWRAPVISUALFLAGS:
		if (vs.SetWrapVisualFlags(static_cast<int>(wParam))) {
			InvalidateStyleRedraw();
			ReconfigureScrollBars();
		}
		break;

	case SCI_GETWRAPVISUALFLAGS:
		return vs.wrap.visualFlags;

	case SCI_SETWRAPVISUALFLAGSLOCATION:
		if (vs.SetWrapVisualFlagsLocation(static_cast<int>(wParam))) {
			InvalidateStyleRedraw();
		}
		break;

	case SCI_GETWRAPVISUALFLAGSLOCATION:
		return vs.wrap.visualFlagsLocation;

	case SCI_SETWRAPSTARTINDENT:
		if (vs.SetWrapVisualStartIndent(static_cast<int>(wParam))) {
			InvalidateStyleRedraw();
			ReconfigureScrollBars();
		}
		break;

	case SCI_GETWRAPSTARTINDENT:
		return vs.wrap.visualStartIndent;

	case SCI_SETWRAPINDENTMODE:
		if (vs.SetWrapIndentMode(static_cast<int>(wParam))) {
			InvalidateStyleRedraw();
			ReconfigureScrollBars();
		}
		break;

	case SCI_GETWRAPINDENTMODE:
		return vs.wrap.indentMode;

	case SCI_SETLAYOUTCACHE:
		if (wParam <= SC_CACHE_DOCUMENT) {
			view.llc.SetLevel(static_cast<LineLayoutCache::Cache>(wParam));
		}
		break;

	case SCI_GETLAYOUTCACHE:
		return static_cast<sptr_t>(view.llc.GetLevel());

	case SCI_SETPOSITIONCACHE:
		view.posCache.SetSize(wParam);
		break;

	case SCI_GETPOSITIONCACHE:
		return view.posCache.GetSize();

	case SCI_SETSCROLLWIDTH:
		PLATFORM_ASSERT(wParam > 0);
		if ((wParam > 0) && (wParam != static_cast<unsigned int>(scrollWidth))) {
			view.lineWidthMaxSeen = 0;
			scrollWidth = static_cast<int>(wParam);
			SetScrollBars();
		}
		break;

	case SCI_GETSCROLLWIDTH:
		return scrollWidth;

	case SCI_SETSCROLLWIDTHTRACKING:
		trackLineWidth = wParam != 0;
		break;

	case SCI_GETSCROLLWIDTHTRACKING:
		return trackLineWidth;

	case SCI_LINESJOIN:
		LinesJoin();
		break;

	case SCI_LINESSPLIT:
		LinesSplit(static_cast<int>(wParam));
		break;

	case SCI_TEXTWIDTH:
		PLATFORM_ASSERT(wParam < vs.styles.size());
		PLATFORM_ASSERT(lParam);
		return TextWidth(wParam, CharPtrFromSPtr(lParam));

	case SCI_TEXTHEIGHT:
		RefreshStyleData();
		return vs.lineHeight;

	case SCI_SETENDATLASTLINE: {
		const int line = std::clamp(static_cast<int>(wParam), 0, 4);
		if (endAtLastLine != line) {
			endAtLastLine = line;
			SetScrollBars();
		}
	}
	break;

	case SCI_GETENDATLASTLINE:
		return endAtLastLine;

	case SCI_SETCARETSTICKY:
		PLATFORM_ASSERT(wParam <= SC_CARETSTICKY_WHITESPACE);
		if (wParam <= SC_CARETSTICKY_WHITESPACE) {
			caretSticky = static_cast<int>(wParam);
		}
		break;

	case SCI_GETCARETSTICKY:
		return caretSticky;

	case SCI_TOGGLECARETSTICKY:
		caretSticky = !caretSticky;
		break;

	case SCI_GETCOLUMN:
		return pdoc->GetColumn(wParam);

	case SCI_FINDCOLUMN:
		return pdoc->FindColumn(wParam, lParam);

	case SCI_SETHSCROLLBAR :
		if (horizontalScrollBarVisible != (wParam != 0)) {
			horizontalScrollBarVisible = wParam != 0;
			SetScrollBars();
			ReconfigureScrollBars();
		}
		break;

	case SCI_GETHSCROLLBAR:
		return horizontalScrollBarVisible;

	case SCI_SETVSCROLLBAR:
		if (verticalScrollBarVisible != (wParam != 0)) {
			verticalScrollBarVisible = wParam != 0;
			SetScrollBars();
			ReconfigureScrollBars();
			if (verticalScrollBarVisible)
				SetVerticalScrollPos();
		}
		break;

	case SCI_GETVSCROLLBAR:
		return verticalScrollBarVisible;

	case SCI_SETINDENTATIONGUIDES:
		vs.viewIndentationGuides = static_cast<IndentView>(wParam);
		Redraw();
		break;

	case SCI_GETINDENTATIONGUIDES:
		return static_cast<sptr_t>(vs.viewIndentationGuides);

	case SCI_SETHIGHLIGHTGUIDE:
		if ((highlightGuideColumn != static_cast<int>(wParam)) || (wParam > 0)) {
			highlightGuideColumn = static_cast<int>(wParam);
			Redraw();
		}
		break;

	case SCI_GETHIGHLIGHTGUIDE:
		return highlightGuideColumn;

	case SCI_GETLINEENDPOSITION:
		return pdoc->LineEnd(wParam);

	case SCI_SETCODEPAGE:
		if (ValidCodePage(static_cast<int>(wParam))) {
			const int oldCodePage = pdoc->dbcsCodePage;
			if (pdoc->SetDBCSCodePage(static_cast<int>(wParam))) {
				pcs->Clear();
				pcs->InsertLines(0, pdoc->LinesTotal() - 1);
				SetAnnotationHeights(0, pdoc->LinesTotal());
				InvalidateStyleRedraw();
				SetRepresentations();
				NotifyCodePageChanged(oldCodePage);
			}
		}
		break;

	case SCI_GETCODEPAGE:
		return pdoc->dbcsCodePage;

	case SCI_SETIMEINTERACTION:
		imeInteraction = static_cast<EditModel::IMEInteraction>(wParam);
		break;

	case SCI_GETIMEINTERACTION:
		return static_cast<sptr_t>(imeInteraction);

	case SCI_SETBIDIRECTIONAL:
		// SCI_SETBIDIRECTIONAL is implemented on platform subclasses if they support bidirectional text.
		break;

	case SCI_GETBIDIRECTIONAL:
		return static_cast<sptr_t>(bidirectional);

	case SCI_GETLINECHARACTERINDEX:
		return pdoc->LineCharacterIndex();

	case SCI_ALLOCATELINECHARACTERINDEX:
		pdoc->AllocateLineCharacterIndex(static_cast<int>(wParam));
		break;

	case SCI_RELEASELINECHARACTERINDEX:
		pdoc->ReleaseLineCharacterIndex(static_cast<int>(wParam));
		break;

	case SCI_LINEFROMINDEXPOSITION:
		return pdoc->LineFromPositionIndex(wParam, static_cast<int>(lParam));

	case SCI_INDEXPOSITIONFROMLINE:
		return pdoc->IndexLineStart(wParam, static_cast<int>(lParam));

		// Marker definition and setting
	case SCI_MARKERDEFINE:
		if (wParam <= MARKER_MAX) {
			vs.markers[wParam].markType = static_cast<int>(lParam);
			vs.CalcLargestMarkerHeight();
		}
		InvalidateStyleData();
		RedrawSelMargin();
		break;

	case SCI_MARKERSYMBOLDEFINED:
		if (wParam <= MARKER_MAX)
			return vs.markers[wParam].markType;
		else
			return 0;

	case SCI_MARKERSETFORE:
		if (wParam <= MARKER_MAX)
			vs.markers[wParam].fore = ColourAlpha::FromRGB(static_cast<unsigned int>(lParam));
		InvalidateStyleData();
		RedrawSelMargin();
		break;
	case SCI_MARKERSETBACK:
		if (wParam <= MARKER_MAX)
			vs.markers[wParam].back = ColourAlpha::FromRGB(static_cast<unsigned int>(lParam));
		InvalidateStyleData();
		RedrawSelMargin();
		break;
	case SCI_MARKERSETBACKSELECTED:
		if (wParam <= MARKER_MAX)
			vs.markers[wParam].backSelected = ColourAlpha::FromRGB(static_cast<unsigned int>(lParam));
		InvalidateStyleData();
		RedrawSelMargin();
		break;
	case SCI_MARKERSETFORETRANSLUCENT:
		if (wParam <= MARKER_MAX)
			vs.markers[wParam].fore = ColourAlpha(static_cast<unsigned int>(lParam));
		InvalidateStyleData();
		RedrawSelMargin();
		break;
	case SCI_MARKERSETBACKTRANSLUCENT:
		if (wParam <= MARKER_MAX)
			vs.markers[wParam].back = ColourAlpha(static_cast<unsigned int>(lParam));
		InvalidateStyleData();
		RedrawSelMargin();
		break;
	case SCI_MARKERSETBACKSELECTEDTRANSLUCENT:
		if (wParam <= MARKER_MAX)
			vs.markers[wParam].backSelected = ColourAlpha(static_cast<unsigned int>(lParam));
		InvalidateStyleData();
		RedrawSelMargin();
		break;
	case SCI_MARKERSETSTROKEWIDTH:
		if (wParam <= MARKER_MAX)
			vs.markers[wParam].strokeWidth = lParam / 100.0f;
		InvalidateStyleData();
		RedrawSelMargin();
		break;
	case SCI_MARKERENABLEHIGHLIGHT:
		marginView.highlightDelimiter.isEnabled = wParam == 1;
		RedrawSelMargin();
		break;
	case SCI_MARKERSETALPHA:
		if (wParam <= MARKER_MAX)
			vs.markers[wParam].alpha = static_cast<int>(lParam);
		InvalidateStyleData();
		RedrawSelMargin();
		break;
	case SCI_MARKERADD: {
			const int markerID = pdoc->AddMark(wParam, static_cast<int>(lParam));
			return markerID;
		}
	case SCI_MARKERADDSET:
		if (lParam != 0)
			pdoc->AddMarkSet(wParam, static_cast<MarkerMask>(lParam));
		break;

	case SCI_MARKERDELETE:
		pdoc->DeleteMark(wParam, static_cast<int>(lParam));
		break;

	case SCI_MARKERDELETEALL:
		pdoc->DeleteAllMarks(static_cast<int>(wParam));
		break;

	case SCI_MARKERGET:
		return pdoc->GetMark(wParam);

	case SCI_MARKERNEXT:
		return pdoc->MarkerNext(wParam, static_cast<MarkerMask>(lParam));

	case SCI_MARKERPREVIOUS: {
			for (Sci::Line iLine = wParam; iLine >= 0; iLine--) {
				if ((pdoc->GetMark(iLine) & lParam) != 0)
					return iLine;
			}
		}
		return -1;

	case SCI_MARKERDEFINEPIXMAP:
		if (wParam <= MARKER_MAX) {
			vs.markers[wParam].SetXPM(CharPtrFromSPtr(lParam));
			vs.CalcLargestMarkerHeight();
		}
		InvalidateStyleData();
		RedrawSelMargin();
		break;

	case SCI_RGBAIMAGESETWIDTH:
		sizeRGBAImage.x = static_cast<XYPOSITION>(wParam);
		break;

	case SCI_RGBAIMAGESETHEIGHT:
		sizeRGBAImage.y = static_cast<XYPOSITION>(wParam);
		break;

	case SCI_RGBAIMAGESETSCALE:
		scaleRGBAImage = static_cast<float>(wParam);
		break;

	case SCI_MARKERDEFINERGBAIMAGE:
		if (wParam <= MARKER_MAX) {
			vs.markers[wParam].SetRGBAImage(sizeRGBAImage, scaleRGBAImage / 100.0f, ConstUCharPtrFromSPtr(lParam));
			vs.CalcLargestMarkerHeight();
		}
		InvalidateStyleData();
		RedrawSelMargin();
		break;

	case SCI_SETMARGINTYPEN:
		if (ValidMargin(wParam)) {
			vs.ms[wParam].style = static_cast<int>(lParam);
			InvalidateStyleRedraw();
		}
		break;

	case SCI_GETMARGINTYPEN:
		if (ValidMargin(wParam))
			return vs.ms[wParam].style;
		else
			return 0;

	case SCI_SETMARGINWIDTHN:
		if (ValidMargin(wParam)) {
			// Short-circuit if the width is unchanged, to avoid unnecessary redraw.
			if (vs.ms[wParam].width != lParam) {
				lastXChosen += static_cast<int>(lParam) - vs.ms[wParam].width;
				vs.ms[wParam].width = static_cast<int>(lParam);
				InvalidateStyleRedraw();
			}
		}
		break;

	case SCI_GETMARGINWIDTHN:
		if (ValidMargin(wParam))
			return vs.ms[wParam].width;
		else
			return 0;

	case SCI_SETMARGINMASKN:
		if (ValidMargin(wParam)) {
			vs.ms[wParam].mask = static_cast<MarkerMask>(lParam);
			InvalidateStyleRedraw();
		}
		break;

	case SCI_GETMARGINMASKN:
		if (ValidMargin(wParam))
			return vs.ms[wParam].mask;
		else
			return 0;

	case SCI_SETMARGINSENSITIVEN:
		if (ValidMargin(wParam)) {
			vs.ms[wParam].sensitive = lParam != 0;
			InvalidateStyleRedraw();
		}
		break;

	case SCI_GETMARGINSENSITIVEN:
		if (ValidMargin(wParam))
			return vs.ms[wParam].sensitive ? 1 : 0;
		else
			return 0;

	case SCI_SETMARGINCURSORN:
		if (ValidMargin(wParam))
			vs.ms[wParam].cursor = static_cast<int>(lParam);
		break;

	case SCI_GETMARGINCURSORN:
		if (ValidMargin(wParam))
			return vs.ms[wParam].cursor;
		else
			return 0;

	case SCI_SETMARGINBACKN:
		if (ValidMargin(wParam)) {
			vs.ms[wParam].back = ColourAlpha::FromRGB(static_cast<unsigned int>(lParam));
			InvalidateStyleRedraw();
		}
		break;

	case SCI_GETMARGINBACKN:
		if (ValidMargin(wParam))
			return vs.ms[wParam].back.OpaqueRGB();
		else
			return 0;

	case SCI_SETMARGINS:
		if (wParam < 1000)
			vs.ms.resize(wParam);
		break;

	case SCI_GETMARGINS:
		return vs.ms.size();

	case SCI_STYLECLEARALL:
		vs.ClearStyles();
		InvalidateStyleRedraw();
		break;

	case SCI_COPYSTYLES:
		vs.CopyStyles(wParam, lParam);
		InvalidateStyleRedraw();
		break;

	case SCI_STYLESETFORE:
	case SCI_STYLESETBACK:
	case SCI_STYLESETBOLD:
	case SCI_STYLESETWEIGHT:
	case SCI_STYLESETITALIC:
	case SCI_STYLESETEOLFILLED:
	case SCI_STYLESETSIZE:
	case SCI_STYLESETSIZEFRACTIONAL:
	case SCI_STYLESETFONT:
	case SCI_STYLESETUNDERLINE:
	case SCI_STYLESETSTRIKE:
	case SCI_STYLESETCASE:
	case SCI_STYLESETCHARACTERSET:
	case SCI_STYLESETVISIBLE:
	case SCI_STYLESETCHANGEABLE:
	case SCI_STYLESETHOTSPOT:
		StyleSetMessage(iMessage, wParam, lParam);
		break;

	case SCI_STYLEGETFORE:
	case SCI_STYLEGETBACK:
	case SCI_STYLEGETBOLD:
	case SCI_STYLEGETWEIGHT:
	case SCI_STYLEGETITALIC:
	case SCI_STYLEGETEOLFILLED:
	case SCI_STYLEGETSIZE:
	case SCI_STYLEGETSIZEFRACTIONAL:
	case SCI_STYLEGETFONT:
	case SCI_STYLEGETUNDERLINE:
	case SCI_STYLEGETCASE:
	case SCI_STYLEGETCHARACTERSET:
	case SCI_STYLEGETVISIBLE:
	case SCI_STYLEGETCHANGEABLE:
	case SCI_STYLEGETHOTSPOT:
		return StyleGetMessage(iMessage, wParam, lParam);

	case SCI_STYLERESETDEFAULT:
		vs.ResetDefaultStyle();
		InvalidateStyleRedraw();
		break;

	case SCI_SETELEMENTCOLOUR:
		vs.elementColours[static_cast<int>(wParam)] = ColourAlpha(static_cast<unsigned int>(lParam));
		break;

	case SCI_GETELEMENTCOLOUR:
		return vs.ElementColour(static_cast<int>(wParam)).value_or(ColourAlpha()).AsInteger();

	case SCI_RESETELEMENTCOLOUR:
		vs.elementColours[static_cast<int>(wParam)].reset();
		break;

	case SCI_GETELEMENTISSET:
		return vs.ElementColour(static_cast<int>(wParam)).has_value();

	case SCI_GETELEMENTALLOWSTRANSLUCENT:
		return vs.ElementAllowsTranslucent(static_cast<int>(wParam));

	case SCI_SETFONTLOCALE:
		if (lParam != 0) {
			vs.SetFontLocaleName(CharPtrFromSPtr(lParam));
		}
		break;

	case SCI_GETFONTLOCALE:
		return StringResult(lParam, vs.localeName.c_str());

	case SCI_SETLINESTATE:
		return pdoc->SetLineState(wParam, static_cast<int>(lParam));

	case SCI_GETLINESTATE:
		return pdoc->GetLineState(wParam);

	case SCI_GETMAXLINESTATE:
		return pdoc->GetMaxLineState();

	case SCI_GETCARETLINEVISIBLE:
		return vs.caretLine.show;
	case SCI_SETCARETLINEVISIBLE:
		vs.caretLine.show = wParam != 0;
		InvalidateStyleRedraw();
		break;
	case SCI_GETCARETLINEVISIBLEALWAYS:
		return vs.caretLine.alwaysShow;
	case SCI_SETCARETLINEVISIBLEALWAYS:
		vs.caretLine.alwaysShow = wParam != 0;
		InvalidateStyleRedraw();
		break;

	case SCI_GETCARETLINEFRAME:
		return vs.caretLine.frame;
	case SCI_SETCARETLINEFRAME:
		vs.caretLine.frame = static_cast<int>(wParam);
		InvalidateStyleRedraw();
		break;
	case SCI_GETCARETLINEBACK:
		return vs.caretLine.background.OpaqueRGB();
	case SCI_SETCARETLINEBACK:
		vs.caretLine.background = ColourAlpha::FromRGB(static_cast<unsigned int>(wParam));
		InvalidateStyleRedraw();
		break;
	case SCI_GETCARETLINEBACKALPHA:
		return vs.caretLine.alpha;
	case SCI_SETCARETLINEBACKALPHA:
		vs.caretLine.alpha = static_cast<int>(wParam);
		InvalidateStyleRedraw();
		break;

		// Folding messages

	case SCI_VISIBLEFROMDOCLINE:
		return pcs->DisplayFromDoc(wParam);

	case SCI_DOCLINEFROMVISIBLE:
		return pcs->DocFromDisplay(wParam);

	case SCI_WRAPCOUNT:
		return WrapCount(wParam);

	case SCI_SETFOLDLEVEL: {
			const int prev = pdoc->SetLevel(wParam, static_cast<int>(lParam));
			if (prev != static_cast<int>(lParam))
				RedrawSelMargin();
			return prev;
		}

	case SCI_GETFOLDLEVEL:
		return pdoc->GetLevel(wParam);

	case SCI_GETLASTCHILD:
		return pdoc->GetLastChild(wParam, static_cast<int>(lParam));

	case SCI_GETFOLDPARENT:
		return pdoc->GetFoldParent(wParam);

	case SCI_SHOWLINES:
		pcs->SetVisible(wParam, lParam, true);
		SetScrollBars();
		Redraw();
		break;

	case SCI_HIDELINES:
		if (wParam > 0)
			pcs->SetVisible(wParam, lParam, false);
		SetScrollBars();
		Redraw();
		break;

	case SCI_GETLINEVISIBLE:
		return pcs->GetVisible(wParam);

	case SCI_GETALLLINESVISIBLE:
		return pcs->HiddenLines() ? 0 : 1;

	case SCI_SETFOLDEXPANDED:
		SetFoldExpanded(wParam, lParam != 0);
		break;

	case SCI_GETFOLDEXPANDED:
		return pcs->GetExpanded(wParam);

	case SCI_SETAUTOMATICFOLD:
		foldAutomatic = static_cast<int>(wParam);
		break;

	case SCI_GETAUTOMATICFOLD:
		return foldAutomatic;

	case SCI_SETFOLDFLAGS:
		foldFlags = static_cast<int>(wParam);
		Redraw();
		break;

	case SCI_TOGGLEFOLDSHOWTEXT:
#if EnablePerLineFoldDisplayText
		pcs->SetFoldDisplayText(wParam, CharPtrFromSPtr(lParam));
#endif
		FoldLine(wParam, SC_FOLDACTION_TOGGLE);
		break;

	case SCI_FOLDDISPLAYTEXTSETSTYLE:
		foldDisplayTextStyle = static_cast<int>(wParam);
		Redraw();
		break;

	case SCI_FOLDDISPLAYTEXTGETSTYLE:
		return foldDisplayTextStyle;

	case SCI_SETDEFAULTFOLDDISPLAYTEXT:
		SetDefaultFoldDisplayText(CharPtrFromSPtr(lParam));
		Redraw();
		break;

	case SCI_GETDEFAULTFOLDDISPLAYTEXT:
		return StringResult(lParam, GetDefaultFoldDisplayText());

	case SCI_TOGGLEFOLD:
		FoldLine(wParam, SC_FOLDACTION_TOGGLE);
		break;

	case SCI_FOLDLINE:
		FoldLine(wParam, static_cast<int>(lParam));
		break;

	case SCI_FOLDCHILDREN:
		FoldExpand(wParam, static_cast<int>(lParam), pdoc->GetLevel(static_cast<int>(wParam)));
		break;

	case SCI_FOLDALL:
		FoldAll(static_cast<int>(wParam));
		break;

	case SCI_EXPANDCHILDREN:
		FoldExpand(wParam, SC_FOLDACTION_EXPAND, static_cast<int>(lParam));
		break;

	case SCI_CONTRACTEDFOLDNEXT:
		return ContractedFoldNext(wParam);

	case SCI_ENSUREVISIBLE:
		EnsureLineVisible(wParam, false);
		break;

	case SCI_ENSUREVISIBLEENFORCEPOLICY:
		EnsureLineVisible(wParam, true);
		break;

	case SCI_SCROLLRANGE:
		ScrollRange(SelectionRange(wParam, lParam));
		break;

	case SCI_SEARCHANCHOR:
		SearchAnchor();
		break;

	case SCI_SEARCHNEXT:
	case SCI_SEARCHPREV:
		return SearchText(iMessage, wParam, lParam);

	case SCI_SETXCARETPOLICY:
		caretPolicies.x = CaretPolicy(wParam, lParam);
		break;

	case SCI_SETYCARETPOLICY:
		caretPolicies.y = CaretPolicy(wParam, lParam);
		break;

	case SCI_SETVISIBLEPOLICY:
		visiblePolicy = CaretPolicy(wParam, lParam);
		break;

	case SCI_LINESONSCREEN:
		return LinesOnScreen();

	case SCI_SETSELFORE:
		vs.selection.colours.fore = OptionalColour(wParam, lParam);
		vs.selection.additionalForeground = ColourAlpha::FromRGB(static_cast<unsigned int>(lParam));
		InvalidateStyleRedraw();
		break;

	case SCI_SETSELBACK:
		vs.selection.colours.back = OptionalColour(wParam, lParam);
		vs.selection.additionalBackground = ColourAlpha::FromRGB(static_cast<unsigned int>(lParam));
		InvalidateStyleRedraw();
		break;

	case SCI_SETSELALPHA:
		vs.selection.alpha = static_cast<int>(wParam);
		vs.selection.additionalAlpha = static_cast<int>(wParam);
		InvalidateStyleRedraw();
		break;

	case SCI_GETSELALPHA:
		return vs.selection.alpha;

	case SCI_GETSELEOLFILLED:
		return vs.selection.eolFilled;

	case SCI_SETSELEOLFILLED:
		vs.selection.eolFilled = wParam != 0;
		InvalidateStyleRedraw();
		break;

	case SCI_SETEOLSELECTEDWIDTH:
		vs.selection.eolSelectedWidth = std::clamp(static_cast<int>(wParam), 0, 100);
		InvalidateStyleRedraw();
		break;

	case SCI_SETWHITESPACEFORE:
		vs.whitespaceColours.fore = OptionalColour(wParam, lParam);
		InvalidateStyleRedraw();
		break;

	case SCI_SETWHITESPACEBACK:
		vs.whitespaceColours.back = OptionalColour(wParam, lParam);
		InvalidateStyleRedraw();
		break;

	case SCI_SETWHITESPACEFOREALPHA:
		vs.whitespaceForeAlpha = static_cast<int>(wParam);
		InvalidateStyleRedraw();
		break;

	case SCI_GETWHITESPACEFOREALPHA:
		return vs.whitespaceForeAlpha;

	case SCI_SETCARETFORE:
		vs.caret.colour = ColourAlpha::FromRGB(static_cast<unsigned int>(wParam));
		vs.caret.additionalColour = ColourAlpha::FromRGB(static_cast<unsigned int>(wParam));
		InvalidateStyleRedraw();
		break;

	case SCI_GETCARETFORE:
		return vs.caret.colour.OpaqueRGB();

	case SCI_SETCARETSTYLE:
		if (wParam <= (CARETSTYLE_BLOCK | CARETSTYLE_OVERSTRIKE_BLOCK | CARETSTYLE_BLOCK_AFTER))
			vs.caret.style = static_cast<int>(wParam);
		else
			/* Default to the line caret */
			vs.caret.style = CARETSTYLE_LINE;
		InvalidateStyleRedraw();
		break;

	case SCI_GETCARETSTYLE:
		return vs.caret.style;

	case SCI_SETCARETWIDTH:
		// Windows accessibility allows 20 pixels caret width.
		vs.caret.width = std::clamp(static_cast<int>(wParam), 0, 20);
		InvalidateStyleRedraw();
		break;

	case SCI_GETCARETWIDTH:
		return vs.caret.width;

	case SCI_ASSIGNCMDKEY:
		kmap.AssignCmdKey(LowShortFromWParam(wParam),
			HighShortFromWParam(wParam), static_cast<unsigned int>(lParam));
		break;

	case SCI_CLEARCMDKEY:
		kmap.AssignCmdKey(LowShortFromWParam(wParam),
			HighShortFromWParam(wParam), SCI_NULL);
		break;

	case SCI_CLEARALLCMDKEYS:
		kmap.Clear();
		break;

	case SCI_INDICSETSTYLE:
		if (wParam <= INDICATOR_MAX) {
			vs.indicators[wParam].sacNormal.style = static_cast<int>(lParam);
			vs.indicators[wParam].sacHover.style = static_cast<int>(lParam);
			InvalidateStyleRedraw();
		}
		break;

	case SCI_INDICGETSTYLE:
		return (wParam <= INDICATOR_MAX) ? vs.indicators[wParam].sacNormal.style : 0;

	case SCI_INDICSETFORE:
		if (wParam <= INDICATOR_MAX) {
			vs.indicators[wParam].sacNormal.fore = ColourAlpha::FromRGB(static_cast<unsigned int>(lParam));
			vs.indicators[wParam].sacHover.fore = ColourAlpha::FromRGB(static_cast<unsigned int>(lParam));
			InvalidateStyleRedraw();
		}
		break;

	case SCI_INDICGETFORE:
		return (wParam <= INDICATOR_MAX) ? vs.indicators[wParam].sacNormal.fore.OpaqueRGB() : 0;

	case SCI_INDICSETHOVERSTYLE:
		if (wParam <= INDICATOR_MAX) {
			vs.indicators[wParam].sacHover.style = static_cast<int>(lParam);
			InvalidateStyleRedraw();
		}
		break;

	case SCI_INDICGETHOVERSTYLE:
		return (wParam <= INDICATOR_MAX) ? vs.indicators[wParam].sacHover.style : 0;

	case SCI_INDICSETHOVERFORE:
		if (wParam <= INDICATOR_MAX) {
			vs.indicators[wParam].sacHover.fore = ColourAlpha::FromRGB(static_cast<unsigned int>(lParam));
			InvalidateStyleRedraw();
		}
		break;

	case SCI_INDICGETHOVERFORE:
		return (wParam <= INDICATOR_MAX) ? vs.indicators[wParam].sacHover.fore.OpaqueRGB() : 0;

	case SCI_INDICSETFLAGS:
		if (wParam <= INDICATOR_MAX) {
			vs.indicators[wParam].SetFlags(static_cast<int>(lParam));
			InvalidateStyleRedraw();
		}
		break;

	case SCI_INDICGETFLAGS:
		return (wParam <= INDICATOR_MAX) ? vs.indicators[wParam].Flags() : 0;

	case SCI_INDICSETUNDER:
		if (wParam <= INDICATOR_MAX) {
			vs.indicators[wParam].under = lParam != 0;
			InvalidateStyleRedraw();
		}
		break;

	case SCI_INDICGETUNDER:
		return (wParam <= INDICATOR_MAX) ? vs.indicators[wParam].under : 0;

	case SCI_INDICSETALPHA:
		if (wParam <= INDICATOR_MAX && lParam >=0 && lParam <= 255) {
			vs.indicators[wParam].fillAlpha = static_cast<int>(lParam);
			InvalidateStyleRedraw();
		}
		break;

	case SCI_INDICGETALPHA:
		return (wParam <= INDICATOR_MAX) ? vs.indicators[wParam].fillAlpha : 0;

	case SCI_INDICSETOUTLINEALPHA:
		if (wParam <= INDICATOR_MAX && lParam >=0 && lParam <= 255) {
			vs.indicators[wParam].outlineAlpha = static_cast<int>(lParam);
			InvalidateStyleRedraw();
		}
		break;

	case SCI_INDICGETOUTLINEALPHA:
		return (wParam <= INDICATOR_MAX) ? vs.indicators[wParam].outlineAlpha : 0;

	case SCI_INDICSETSTROKEWIDTH:
		if (wParam <= INDICATOR_MAX && lParam >= 0 && lParam <= 1000) {
			vs.indicators[wParam].strokeWidth = lParam / 100.0f;
			InvalidateStyleRedraw();
		}
		break;

	case SCI_INDICGETSTROKEWIDTH:
		if (wParam <= INDICATOR_MAX) {
			return std::lround(vs.indicators[wParam].strokeWidth * 100);
		}
		break;

	case SCI_SETINDICATORCURRENT:
		pdoc->DecorationSetCurrentIndicator(static_cast<int>(wParam));
		break;
	case SCI_GETINDICATORCURRENT:
		return pdoc->decorations->GetCurrentIndicator();
	case SCI_SETINDICATORVALUE:
		pdoc->decorations->SetCurrentValue(static_cast<int>(wParam));
		break;
	case SCI_GETINDICATORVALUE:
		return pdoc->decorations->GetCurrentValue();

	case SCI_INDICATORFILLRANGE:
		pdoc->DecorationFillRange(wParam,
			pdoc->decorations->GetCurrentValue(), lParam);
		break;

	case SCI_INDICATORCLEARRANGE:
		pdoc->DecorationFillRange(wParam, 0, lParam);
		break;

	case SCI_INDICATORALLONFOR:
		return pdoc->decorations->AllOnFor(wParam);

	case SCI_INDICATORVALUEAT:
		return pdoc->decorations->ValueAt(static_cast<int>(wParam), lParam);

	case SCI_INDICATORSTART:
		return pdoc->decorations->Start(static_cast<int>(wParam), lParam);

	case SCI_INDICATOREND:
		return pdoc->decorations->End(static_cast<int>(wParam), lParam);

	case SCI_LINECOPY: {
		const Sci::Line lineStart = pdoc->SciLineFromPosition(SelectionStart().Position());
		const Sci::Line lineEnd = pdoc->SciLineFromPosition(SelectionEnd().Position());
		CopyRangeToClipboard(pdoc->LineStart(lineStart), pdoc->LineStart(lineEnd + 1), wParam != 0);
	}
	break;
	case SCI_LINECUT: {
		const Sci::Line lineStart = pdoc->SciLineFromPosition(SelectionStart().Position());
		const Sci::Line lineEnd = pdoc->SciLineFromPosition(SelectionEnd().Position());
		const Sci::Position start = pdoc->LineStart(lineStart);
		const Sci::Position end = pdoc->LineStart(lineEnd + 1);
		SetSelection(start, end);
		Cut(false, wParam != 0);
		SetLastXChosen();
	}
	break;

	case SCI_LINEDOWN:
	case SCI_LINEDOWNEXTEND:
	case SCI_PARADOWN:
	case SCI_PARADOWNEXTEND:
	case SCI_LINEUP:
	case SCI_LINEUPEXTEND:
	case SCI_PARAUP:
	case SCI_PARAUPEXTEND:
	case SCI_CHARLEFT:
	case SCI_CHARLEFTEXTEND:
	case SCI_CHARRIGHT:
	case SCI_CHARRIGHTEXTEND:
	case SCI_WORDLEFT:
	case SCI_WORDLEFTEXTEND:
	case SCI_WORDRIGHT:
	case SCI_WORDRIGHTEXTEND:
	case SCI_WORDLEFTEND:
	case SCI_WORDLEFTENDEXTEND:
	case SCI_WORDRIGHTEND:
	case SCI_WORDRIGHTENDEXTEND:
	case SCI_HOME:
	case SCI_HOMEEXTEND:
	case SCI_LINEEND:
	case SCI_LINEENDEXTEND:
	case SCI_HOMEWRAP:
	case SCI_HOMEWRAPEXTEND:
	case SCI_LINEENDWRAP:
	case SCI_LINEENDWRAPEXTEND:
	case SCI_DOCUMENTSTART:
	case SCI_DOCUMENTSTARTEXTEND:
	case SCI_DOCUMENTEND:
	case SCI_DOCUMENTENDEXTEND:
	case SCI_SCROLLTOSTART:
	case SCI_SCROLLTOEND:

	case SCI_STUTTEREDPAGEUP:
	case SCI_STUTTEREDPAGEUPEXTEND:
	case SCI_STUTTEREDPAGEDOWN:
	case SCI_STUTTEREDPAGEDOWNEXTEND:

	case SCI_PAGEUP:
	case SCI_PAGEUPEXTEND:
	case SCI_PAGEDOWN:
	case SCI_PAGEDOWNEXTEND:
	case SCI_EDITTOGGLEOVERTYPE:
	case SCI_CANCEL:
	case SCI_DELETEBACK:
	case SCI_TAB:
	case SCI_BACKTAB:
	case SCI_NEWLINE:
	case SCI_FORMFEED:
	case SCI_VCHOME:
	case SCI_VCHOMEEXTEND:
	case SCI_VCHOMEWRAP:
	case SCI_VCHOMEWRAPEXTEND:
	case SCI_VCHOMEDISPLAY:
	case SCI_VCHOMEDISPLAYEXTEND:
	case SCI_ZOOMIN:
	case SCI_ZOOMOUT:
	case SCI_DELWORDLEFT:
	case SCI_DELWORDRIGHT:
	case SCI_DELWORDRIGHTEND:
	case SCI_DELLINELEFT:
	case SCI_DELLINERIGHT:
	case SCI_LINEDELETE:
	case SCI_LINETRANSPOSE:
	case SCI_LINEREVERSE:
	case SCI_LINEDUPLICATE:
	case SCI_LOWERCASE:
	case SCI_UPPERCASE:
	case SCI_LINESCROLLDOWN:
	case SCI_LINESCROLLUP:
	case SCI_WORDPARTLEFT:
	case SCI_WORDPARTLEFTEXTEND:
	case SCI_WORDPARTRIGHT:
	case SCI_WORDPARTRIGHTEXTEND:
	case SCI_DELETEBACKNOTLINE:
	case SCI_HOMEDISPLAY:
	case SCI_HOMEDISPLAYEXTEND:
	case SCI_LINEENDDISPLAY:
	case SCI_LINEENDDISPLAYEXTEND:
	case SCI_LINEDOWNRECTEXTEND:
	case SCI_LINEUPRECTEXTEND:
	case SCI_CHARLEFTRECTEXTEND:
	case SCI_CHARRIGHTRECTEXTEND:
	case SCI_HOMERECTEXTEND:
	case SCI_VCHOMERECTEXTEND:
	case SCI_LINEENDRECTEXTEND:
	case SCI_PAGEUPRECTEXTEND:
	case SCI_PAGEDOWNRECTEXTEND:
	case SCI_SELECTIONDUPLICATE:
		return KeyCommand(iMessage);

	case SCI_BRACEHIGHLIGHT:
		SetBraceHighlight(wParam, lParam, STYLE_BRACELIGHT);
		break;

	case SCI_BRACEHIGHLIGHTINDICATOR:
		if (lParam >= 0 && lParam <= INDICATOR_MAX) {
			vs.braceHighlightIndicatorSet = wParam != 0;
			vs.braceHighlightIndicator = static_cast<int>(lParam);
		}
		break;

	case SCI_BRACEBADLIGHT:
		SetBraceHighlight(wParam, -1, STYLE_BRACEBAD);
		break;

	case SCI_BRACEBADLIGHTINDICATOR:
		if (lParam >= 0 && lParam <= INDICATOR_MAX) {
			vs.braceBadLightIndicatorSet = wParam != 0;
			vs.braceBadLightIndicator = static_cast<int>(lParam);
		}
		break;

	case SCI_BRACEMATCH:
		// wParam is position of char to find brace for,
		// lParam is maximum amount of text to restyle to find it
		return pdoc->BraceMatch(wParam, lParam, 0, false);

	case SCI_BRACEMATCHNEXT:
		return pdoc->BraceMatch(wParam, 0, lParam, true);

	case SCI_GETVIEWEOL:
		return vs.viewEOL;

	case SCI_SETVIEWEOL:
		vs.viewEOL = wParam != 0;
		InvalidateStyleRedraw();
		break;

	case SCI_SETZOOM: {
		const int zoomLevel = std::clamp(static_cast<int>(wParam), SC_MIN_ZOOM_LEVEL, SC_MAX_ZOOM_LEVEL);
		if (zoomLevel != vs.zoomLevel) {
			vs.zoomLevel = zoomLevel;
			vs.fontsValid = false;
			InvalidateStyleRedraw();
			NotifyZoom();
		}
		break;
	}

	case SCI_GETZOOM:
		return vs.zoomLevel;

	case SCI_GETEDGECOLUMN:
		return vs.theEdge.column;

	case SCI_SETEDGECOLUMN:
		vs.theEdge.column = static_cast<int>(wParam);
		InvalidateStyleRedraw();
		break;

	case SCI_GETEDGEMODE:
		return vs.edgeState;

	case SCI_SETEDGEMODE:
		vs.edgeState = static_cast<int>(wParam);
		InvalidateStyleRedraw();
		break;

	case SCI_GETEDGECOLOUR:
		return vs.theEdge.colour.OpaqueRGB();

	case SCI_SETEDGECOLOUR:
		vs.theEdge.colour = ColourAlpha::FromRGB(static_cast<unsigned int>(wParam));
		InvalidateStyleRedraw();
		break;

	case SCI_MULTIEDGEADDLINE:
		vs.AddMultiEdge(wParam, lParam);
		InvalidateStyleRedraw();
		break;

	case SCI_MULTIEDGECLEARALL:
		std::vector<EdgeProperties>().swap(vs.theMultiEdge); // Free vector and memory, C++03 compatible
		InvalidateStyleRedraw();
		break;

	case SCI_GETMULTIEDGECOLUMN: {
		const size_t which = wParam;
		// size_t is unsigned so this also handles negative inputs.
		if (which >= vs.theMultiEdge.size()) {
			return -1;
		}
		return vs.theMultiEdge[which].column;
	}

	case SCI_GETACCESSIBILITY:
		return SC_ACCESSIBILITY_DISABLED;

	case SCI_SETACCESSIBILITY:
		// May be implemented by platform code.
		break;

	case SCI_GETDOCPOINTER:
		return reinterpret_cast<sptr_t>(pdoc);

	case SCI_SETDOCPOINTER:
		CancelModes();
		SetDocPointer(static_cast<Document *>(PtrFromSPtr(lParam)));
		return 0;

	case SCI_CREATEDOCUMENT: {
			Document *doc = new Document(static_cast<int>(lParam));
			doc->AddRef();
			doc->Allocate(wParam);
			pcs = ContractionStateCreate(pdoc->IsLarge());
			return reinterpret_cast<sptr_t>(doc);
		}

	case SCI_ADDREFDOCUMENT:
		(static_cast<Document *>(PtrFromSPtr(lParam)))->AddRef();
		break;

	case SCI_RELEASEDOCUMENT:
		(static_cast<Document *>(PtrFromSPtr(lParam)))->Release();
		break;

	case SCI_GETDOCUMENTOPTIONS:
		return pdoc->Options();

	case SCI_CREATELOADER: {
			Document *doc = new Document(static_cast<int>(lParam));
			doc->AddRef();
			doc->Allocate(wParam);
			doc->SetUndoCollection(false);
			pcs = ContractionStateCreate(pdoc->IsLarge());
			return reinterpret_cast<sptr_t>(doc);
		}

	case SCI_SETMODEVENTMASK:
		modEventMask = static_cast<int>(wParam);
		return 0;

	case SCI_GETMODEVENTMASK:
		return modEventMask;

	case SCI_SETCOMMANDEVENTS:
		commandEvents = wParam != 0;
		return 0;

	case SCI_GETCOMMANDEVENTS:
		return commandEvents;

	case SCI_CONVERTEOLS:
		pdoc->ConvertLineEnds(static_cast<int>(wParam));
		SetSelection(sel.MainCaret(), sel.MainAnchor());	// Ensure selection inside document
		return 0;

	case SCI_SETLENGTHFORENCODE:
		lengthForEncode = wParam;
		return 0;

	case SCI_SELECTIONISRECTANGLE:
		return sel.selType == Selection::SelTypes::rectangle ? 1 : 0;

	case SCI_SETSELECTIONMODE: {
			switch (wParam) {
			case SC_SEL_STREAM:
				sel.SetMoveExtends(!sel.MoveExtends() || (sel.selType != Selection::SelTypes::stream));
				sel.selType = Selection::SelTypes::stream;
				break;
			case SC_SEL_RECTANGLE:
				sel.SetMoveExtends(!sel.MoveExtends() || (sel.selType != Selection::SelTypes::rectangle));
				sel.selType = Selection::SelTypes::rectangle;
				sel.Rectangular() = sel.RangeMain(); // adjust current selection
				break;
			case SC_SEL_LINES:
				sel.SetMoveExtends(!sel.MoveExtends() || (sel.selType != Selection::SelTypes::lines));
				sel.selType = Selection::SelTypes::lines;
				SetSelection(sel.RangeMain().caret, sel.RangeMain().anchor); // adjust current selection
				break;
			case SC_SEL_THIN:
				sel.SetMoveExtends(!sel.MoveExtends() || (sel.selType != Selection::SelTypes::thin));
				sel.selType = Selection::SelTypes::thin;
				break;
			default:
				sel.SetMoveExtends(!sel.MoveExtends() || (sel.selType != Selection::SelTypes::stream));
				sel.selType = Selection::SelTypes::stream;
			}
			InvalidateWholeSelection();
			break;
		}
	case SCI_GETSELECTIONMODE:
		switch (sel.selType) {
		case Selection::SelTypes::stream:
			return SC_SEL_STREAM;
		case Selection::SelTypes::rectangle:
			return SC_SEL_RECTANGLE;
		case Selection::SelTypes::lines:
			return SC_SEL_LINES;
		case Selection::SelTypes::thin:
			return SC_SEL_THIN;
		default:	// ?!
			return SC_SEL_STREAM;
		}
	case SCI_GETMOVEEXTENDSSELECTION:
		return sel.MoveExtends();
	case SCI_GETLINESELSTARTPOSITION:
	case SCI_GETLINESELENDPOSITION: {
			const SelectionSegment segmentLine(
				SelectionPosition(pdoc->LineStart(wParam)),
				SelectionPosition(pdoc->LineEnd(wParam)));
			for (size_t r=0; r<sel.Count(); r++) {
				const SelectionSegment portion = sel.Range(r).Intersect(segmentLine);
				if (portion.start.IsValid()) {
					return (iMessage == SCI_GETLINESELSTARTPOSITION) ? portion.start.Position() : portion.end.Position();
				}
			}
			return INVALID_POSITION;
		}

	case SCI_SETOVERTYPE:
		if (inOverstrike != (wParam != 0)) {
			inOverstrike = wParam != 0;
			ContainerNeedsUpdate(SC_UPDATE_SELECTION);
			ShowCaretAtCurrentPosition();
			SetIdle(true);
		}
		break;

	case SCI_GETOVERTYPE:
		return inOverstrike ? 1 : 0;

	case SCI_SETFOCUS:
		SetFocusState(wParam != 0);
		break;

	case SCI_GETFOCUS:
		return hasFocus;

	case SCI_SETSTATUS:
		errorStatus = static_cast<int>(wParam);
		break;

	case SCI_GETSTATUS:
		return errorStatus;

	case SCI_SETMOUSEDOWNCAPTURES:
		mouseDownCaptures = wParam != 0;
		break;

	case SCI_GETMOUSEDOWNCAPTURES:
		return mouseDownCaptures;

	case SCI_SETMOUSEWHEELCAPTURES:
		mouseWheelCaptures = wParam != 0;
		break;

	case SCI_GETMOUSEWHEELCAPTURES:
		return mouseWheelCaptures;

	case SCI_SETCURSOR:
		cursorMode = static_cast<int>(wParam);
		DisplayCursor(Window::Cursor::text);
		break;

	case SCI_GETCURSOR:
		return cursorMode;

	case SCI_SETCONTROLCHARSYMBOL:
		vs.controlCharSymbol = static_cast<int>(wParam);
		InvalidateStyleRedraw();
		break;

	case SCI_GETCONTROLCHARSYMBOL:
		return vs.controlCharSymbol;

	case SCI_SETREPRESENTATION:
		reprs.SetRepresentation(ConstCharPtrFromUPtr(wParam), ConstCharPtrFromSPtr(lParam));
		break;

	case SCI_GETREPRESENTATION: {
			const Representation *repr = reprs.RepresentationFromCharacter(
				ConstCharPtrFromUPtr(wParam), UTF8MaxBytes);
			if (repr) {
				return StringResult(lParam, repr->stringRep.c_str());
			}
			return 0;
		}

	case SCI_CLEARREPRESENTATION:
		reprs.ClearRepresentation(ConstCharPtrFromUPtr(wParam));
		break;

	case SCI_STARTRECORD:
		recordingMacro = true;
		return 0;

	case SCI_STOPRECORD:
		recordingMacro = false;
		return 0;

	case SCI_MOVECARETINSIDEVIEW:
		MoveCaretInsideView();
		break;

	case SCI_SETFOLDMARGINCOLOUR:
		vs.foldmarginColour = OptionalColour(wParam, lParam);
		InvalidateStyleRedraw();
		break;

	case SCI_SETFOLDMARGINHICOLOUR:
		vs.foldmarginHighlightColour = OptionalColour(wParam, lParam);
		InvalidateStyleRedraw();
		break;

	case SCI_SETHOTSPOTACTIVEFORE:
		vs.hotspotColours.fore = OptionalColour(wParam, lParam);
		InvalidateStyleRedraw();
		break;

	case SCI_GETHOTSPOTACTIVEFORE:
		return vs.hotspotColours.fore.value_or(ColourAlpha()).OpaqueRGB();

	case SCI_SETHOTSPOTACTIVEBACK:
		vs.hotspotColours.back = OptionalColour(wParam, lParam);
		InvalidateStyleRedraw();
		break;

	case SCI_GETHOTSPOTACTIVEBACK:
		return vs.hotspotColours.back.value_or(ColourAlpha()).OpaqueRGB();

	case SCI_SETHOTSPOTACTIVEUNDERLINE:
		vs.hotspotUnderline = wParam != 0;
		InvalidateStyleRedraw();
		break;

	case SCI_GETHOTSPOTACTIVEUNDERLINE:
		return vs.hotspotUnderline ? 1 : 0;

	case SCI_SETHOTSPOTSINGLELINE:
		hotspotSingleLine = wParam != 0;
		InvalidateStyleRedraw();
		break;

	case SCI_GETHOTSPOTSINGLELINE:
		return hotspotSingleLine ? 1 : 0;

	case SCI_SETPASTECONVERTENDINGS:
		convertPastes = wParam != 0;
		break;

	case SCI_GETPASTECONVERTENDINGS:
		return convertPastes ? 1 : 0;

	case SCI_GETCHARACTERPOINTER:
		return reinterpret_cast<sptr_t>(pdoc->BufferPointer());

	case SCI_GETRANGEPOINTER:
		return reinterpret_cast<sptr_t>(pdoc->RangePointer(wParam, lParam));

	case SCI_GETGAPPOSITION:
		return pdoc->GapPosition();

	case SCI_SETEXTRAASCENT:
		vs.extraAscent = static_cast<int>(wParam);
		InvalidateStyleRedraw();
		break;

	case SCI_GETEXTRAASCENT:
		return vs.extraAscent;

	case SCI_SETEXTRADESCENT:
		vs.extraDescent = static_cast<int>(wParam);
		InvalidateStyleRedraw();
		break;

	case SCI_GETEXTRADESCENT:
		return vs.extraDescent;

	case SCI_MARGINSETSTYLEOFFSET:
		vs.marginStyleOffset = static_cast<int>(wParam);
		InvalidateStyleRedraw();
		break;

	case SCI_MARGINGETSTYLEOFFSET:
		return vs.marginStyleOffset;

	case SCI_SETMARGINOPTIONS:
		marginOptions = static_cast<int>(wParam);
		break;

	case SCI_GETMARGINOPTIONS:
		return marginOptions;

	case SCI_MARGINSETTEXT:
		pdoc->MarginSetText(wParam, CharPtrFromSPtr(lParam));
		break;

	case SCI_MARGINGETTEXT: {
			const StyledText st = pdoc->MarginStyledText(wParam);
			return BytesResult(lParam, reinterpret_cast<const unsigned char *>(st.text), st.length);
		}

	case SCI_MARGINSETSTYLE:
		pdoc->MarginSetStyle(wParam, static_cast<int>(lParam));
		break;

	case SCI_MARGINGETSTYLE: {
			const StyledText st = pdoc->MarginStyledText(wParam);
			return st.style;
		}

	case SCI_MARGINSETSTYLES:
		pdoc->MarginSetStyles(wParam, ConstUCharPtrFromSPtr(lParam));
		break;

	case SCI_MARGINGETSTYLES: {
			const StyledText st = pdoc->MarginStyledText(wParam);
			return BytesResult(lParam, st.styles, st.length);
		}

	case SCI_MARGINTEXTCLEARALL:
		pdoc->MarginClearAll();
		break;

	case SCI_ANNOTATIONSETTEXT:
		pdoc->AnnotationSetText(wParam, CharPtrFromSPtr(lParam));
		break;

	case SCI_ANNOTATIONGETTEXT: {
			const StyledText st = pdoc->AnnotationStyledText(wParam);
			return BytesResult(lParam, reinterpret_cast<const unsigned char *>(st.text), st.length);
		}

	case SCI_ANNOTATIONGETSTYLE: {
			const StyledText st = pdoc->AnnotationStyledText(wParam);
			return st.style;
		}

	case SCI_ANNOTATIONSETSTYLE:
		pdoc->AnnotationSetStyle(wParam, static_cast<int>(lParam));
		break;

	case SCI_ANNOTATIONSETSTYLES:
		pdoc->AnnotationSetStyles(wParam, ConstUCharPtrFromSPtr(lParam));
		break;

	case SCI_ANNOTATIONGETSTYLES: {
			const StyledText st = pdoc->AnnotationStyledText(wParam);
			return BytesResult(lParam, st.styles, st.length);
		}

	case SCI_ANNOTATIONGETLINES:
		return pdoc->AnnotationLines(wParam);

	case SCI_ANNOTATIONCLEARALL:
		pdoc->AnnotationClearAll();
		break;

	case SCI_ANNOTATIONSETVISIBLE:
		SetAnnotationVisible(static_cast<int>(wParam));
		break;

	case SCI_ANNOTATIONGETVISIBLE:
		return vs.annotationVisible;

	case SCI_ANNOTATIONSETSTYLEOFFSET:
		vs.annotationStyleOffset = static_cast<int>(wParam);
		InvalidateStyleRedraw();
		break;

	case SCI_ANNOTATIONGETSTYLEOFFSET:
		return vs.annotationStyleOffset;

	case SCI_EOLANNOTATIONSETTEXT:
		pdoc->EOLAnnotationSetText(wParam, CharPtrFromSPtr(lParam));
		break;

	case SCI_EOLANNOTATIONGETTEXT: {
		const StyledText st = pdoc->EOLAnnotationStyledText(wParam);
		return BytesResult(lParam, reinterpret_cast<const unsigned char *>(st.text), st.length);
	}

	case SCI_EOLANNOTATIONGETSTYLE: {
		const StyledText st = pdoc->EOLAnnotationStyledText(wParam);
		return st.style;
	}

	case SCI_EOLANNOTATIONSETSTYLE:
		pdoc->EOLAnnotationSetStyle(wParam, static_cast<int>(lParam));
		break;

	case SCI_EOLANNOTATIONCLEARALL:
		pdoc->EOLAnnotationClearAll();
		break;

	case SCI_EOLANNOTATIONSETVISIBLE:
		SetEOLAnnotationVisible(static_cast<int>(wParam));
		break;

	case SCI_EOLANNOTATIONGETVISIBLE:
		return vs.eolAnnotationVisible;

	case SCI_EOLANNOTATIONSETSTYLEOFFSET:
		vs.eolAnnotationStyleOffset = static_cast<int>(wParam);
		InvalidateStyleRedraw();
		break;

	case SCI_EOLANNOTATIONGETSTYLEOFFSET:
		return vs.eolAnnotationStyleOffset;

	case SCI_RELEASEALLEXTENDEDSTYLES:
		vs.ReleaseAllExtendedStyles();
		break;

	case SCI_ALLOCATEEXTENDEDSTYLES:
		return vs.AllocateExtendedStyles(static_cast<int>(wParam));

	case SCI_SUPPORTSFEATURE:
		return SupportsFeature(static_cast<int>(wParam));

	case SCI_ADDUNDOACTION:
		pdoc->AddUndoAction(wParam, lParam & UNDO_MAY_COALESCE);
		break;

	case SCI_SETMOUSESELECTIONRECTANGULARSWITCH:
		mouseSelectionRectangularSwitch = wParam != 0;
		break;

	case SCI_GETMOUSESELECTIONRECTANGULARSWITCH:
		return mouseSelectionRectangularSwitch;

	case SCI_SETMULTIPLESELECTION:
		multipleSelection = wParam != 0;
		InvalidateCaret();
		break;

	case SCI_GETMULTIPLESELECTION:
		return multipleSelection;

	case SCI_SETADDITIONALSELECTIONTYPING:
		additionalSelectionTyping = wParam != 0;
		InvalidateCaret();
		break;

	case SCI_GETADDITIONALSELECTIONTYPING:
		return additionalSelectionTyping;

	case SCI_SETMULTIPASTE:
		multiPasteMode = static_cast<int>(wParam);
		break;

	case SCI_GETMULTIPASTE:
		return multiPasteMode;

	case SCI_SETADDITIONALCARETSBLINK:
		view.additionalCaretsBlink = wParam != 0;
		InvalidateCaret();
		break;

	case SCI_GETADDITIONALCARETSBLINK:
		return view.additionalCaretsBlink;

	case SCI_SETADDITIONALCARETSVISIBLE:
		view.additionalCaretsVisible = wParam != 0;
		InvalidateCaret();
		break;

	case SCI_GETADDITIONALCARETSVISIBLE:
		return view.additionalCaretsVisible;

	case SCI_GETSELECTIONS:
		return sel.Count();

	case SCI_GETSELECTIONEMPTY:
		return sel.Empty();

	case SCI_CLEARSELECTIONS:
		sel.Clear();
		ContainerNeedsUpdate(SC_UPDATE_SELECTION);
		Redraw();
		break;

	case SCI_SETSELECTION:
		sel.SetSelection(SelectionRange(wParam, lParam));
		Redraw();
		break;

	case SCI_ADDSELECTION:
		sel.AddSelection(SelectionRange(wParam, lParam));
		ContainerNeedsUpdate(SC_UPDATE_SELECTION);
		Redraw();
		break;

	case SCI_DROPSELECTIONN:
		sel.DropSelection(wParam);
		ContainerNeedsUpdate(SC_UPDATE_SELECTION);
		Redraw();
		break;

	case SCI_SETMAINSELECTION:
		sel.SetMain(wParam);
		ContainerNeedsUpdate(SC_UPDATE_SELECTION);
		Redraw();
		break;

	case SCI_GETMAINSELECTION:
		return sel.Main();

	case SCI_SETSELECTIONNCARET:
	case SCI_SETSELECTIONNANCHOR:
	case SCI_SETSELECTIONNCARETVIRTUALSPACE:
	case SCI_SETSELECTIONNANCHORVIRTUALSPACE:
	case SCI_SETSELECTIONNSTART:
	case SCI_SETSELECTIONNEND:
		SetSelectionNMessage(iMessage, wParam, lParam);
		break;

	case SCI_GETSELECTIONNCARET:
		return sel.Range(wParam).caret.Position();

	case SCI_GETSELECTIONNANCHOR:
		return sel.Range(wParam).anchor.Position();

	case SCI_GETSELECTIONNCARETVIRTUALSPACE:
		return sel.Range(wParam).caret.VirtualSpace();

	case SCI_GETSELECTIONNANCHORVIRTUALSPACE:
		return sel.Range(wParam).anchor.VirtualSpace();

	case SCI_GETSELECTIONNSTART:
		return sel.Range(wParam).Start().Position();

	case SCI_GETSELECTIONNSTARTVIRTUALSPACE:
		return sel.Range(wParam).Start().VirtualSpace();

	case SCI_GETSELECTIONNEND:
		return sel.Range(wParam).End().Position();

	case SCI_GETSELECTIONNENDVIRTUALSPACE:
		return sel.Range(wParam).End().VirtualSpace();

	case SCI_SETRECTANGULARSELECTIONCARET:
		if (!sel.IsRectangular())
			sel.Clear();
		sel.selType = Selection::SelTypes::rectangle;
		sel.Rectangular().caret.SetPosition(wParam);
		SetRectangularRange();
		Redraw();
		break;

	case SCI_GETRECTANGULARSELECTIONCARET:
		return sel.Rectangular().caret.Position();

	case SCI_SETRECTANGULARSELECTIONANCHOR:
		if (!sel.IsRectangular())
			sel.Clear();
		sel.selType = Selection::SelTypes::rectangle;
		sel.Rectangular().anchor.SetPosition(wParam);
		SetRectangularRange();
		Redraw();
		break;

	case SCI_GETRECTANGULARSELECTIONANCHOR:
		return sel.Rectangular().anchor.Position();

	case SCI_SETRECTANGULARSELECTIONCARETVIRTUALSPACE:
		if (!sel.IsRectangular())
			sel.Clear();
		sel.selType = Selection::SelTypes::rectangle;
		sel.Rectangular().caret.SetVirtualSpace(wParam);
		SetRectangularRange();
		Redraw();
		break;

	case SCI_GETRECTANGULARSELECTIONCARETVIRTUALSPACE:
		return sel.Rectangular().caret.VirtualSpace();

	case SCI_SETRECTANGULARSELECTIONANCHORVIRTUALSPACE:
		if (!sel.IsRectangular())
			sel.Clear();
		sel.selType = Selection::SelTypes::rectangle;
		sel.Rectangular().anchor.SetVirtualSpace(wParam);
		SetRectangularRange();
		Redraw();
		break;

	case SCI_GETRECTANGULARSELECTIONANCHORVIRTUALSPACE:
		return sel.Rectangular().anchor.VirtualSpace();

	case SCI_SETVIRTUALSPACEOPTIONS:
		virtualSpaceOptions = static_cast<int>(wParam);
		break;

	case SCI_GETVIRTUALSPACEOPTIONS:
		return virtualSpaceOptions;

	case SCI_SETADDITIONALSELFORE:
		vs.selection.additionalForeground = ColourAlpha::FromRGB(static_cast<unsigned int>(wParam));
		InvalidateStyleRedraw();
		break;

	case SCI_SETADDITIONALSELBACK:
		vs.selection.additionalBackground = ColourAlpha::FromRGB(static_cast<unsigned int>(wParam));
		InvalidateStyleRedraw();
		break;

	case SCI_SETADDITIONALSELALPHA:
		vs.selection.additionalAlpha = static_cast<int>(wParam);
		InvalidateStyleRedraw();
		break;

	case SCI_GETADDITIONALSELALPHA:
		return vs.selection.additionalAlpha;

	case SCI_SETADDITIONALCARETFORE:
		vs.caret.additionalColour = ColourAlpha::FromRGB(static_cast<unsigned int>(wParam));
		InvalidateStyleRedraw();
		break;

	case SCI_GETADDITIONALCARETFORE:
		return vs.caret.additionalColour.OpaqueRGB();

	case SCI_ROTATESELECTION:
		sel.RotateMain();
		InvalidateWholeSelection();
		break;

	case SCI_SWAPMAINANCHORCARET:
		InvalidateSelection(sel.RangeMain());
		sel.RangeMain().Swap();
		break;

	case SCI_MULTIPLESELECTADDNEXT:
		MultipleSelectAdd(AddNumber::one);
		break;

	case SCI_MULTIPLESELECTADDEACH:
		MultipleSelectAdd(AddNumber::each);
		break;

	case SCI_CHANGELEXERSTATE:
		pdoc->ChangeLexerState(wParam, lParam);
		break;

	case SCI_SETIDENTIFIER:
		SetCtrlID(static_cast<int>(wParam));
		break;

	case SCI_GETIDENTIFIER:
		return GetCtrlID();

	case SCI_SETTECHNOLOGY:
		// No action by default
		break;

	case SCI_GETTECHNOLOGY:
		return technology;

	case SCI_COUNTCHARACTERS:
		return pdoc->CountCharacters(wParam, lParam);

	case SCI_COUNTCHARACTERSANDCOLUMNS:
		pdoc->CountCharactersAndColumns(static_cast<Sci_TextToFind *>(PtrFromSPtr(lParam)));
		break;

	case SCI_COUNTCODEUNITS:
		return pdoc->CountUTF16(wParam, lParam);

	default:
		return DefWndProc(iMessage, wParam, lParam);
	}
	//Platform::DebugPrintf("end wnd proc\n");
	return 0;
}
