// Scintilla source code edit control
/** @file PlatWin.cxx
 ** Implementation of platform facilities on Windows.
 **/
// Copyright 1998-2003 by Neil Hodgson <neilh@scintilla.org>
// The License.txt file describes the conditions under which this software may be distributed.

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <cmath>
#include <climits>

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <optional>
#include <algorithm>
#include <iterator>
#include <memory>
//#include <mutex>

#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <windowsx.h>
#include <shlwapi.h>

#include "Debugging.h"
#include "Geometry.h"
#include "Platform.h"
#include "VectorISA.h"
#include "GraphicUtils.h"

#include "Scintilla.h"
#include "XPM.h"
#include "CharClassify.h"
#include "UniConversion.h"
#include "FontQuality.h"

#include "PlatWin.h"

#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32	0x00000800
#endif

#if _WIN32_WINNT < _WIN32_WINNT_WIN8
#if NP2_FORCE_COMPILE_C_AS_CPP
extern DWORD kSystemLibraryLoadFlags;
#else
extern "C" DWORD kSystemLibraryLoadFlags;
#endif
#else
#define kSystemLibraryLoadFlags		LOAD_LIBRARY_SEARCH_SYSTEM32
#endif

#if NP2_FORCE_COMPILE_C_AS_CPP
extern UINT g_uSystemDPI;
#else
extern "C" UINT g_uSystemDPI;
#endif

#if !NP2_HAS_GETDPIFORWINDOW
namespace {

using GetDpiForWindowSig = UINT (WINAPI *)(HWND hwnd);
GetDpiForWindowSig fnGetDpiForWindow = nullptr;

#ifndef DPI_ENUMS_DECLARED
#define MDT_EFFECTIVE_DPI	0
#endif

using GetDpiForMonitorSig = HRESULT (WINAPI *)(HMONITOR hmonitor, /*MONITOR_DPI_TYPE*/int dpiType, UINT *dpiX, UINT *dpiY);
HMODULE hShcoreDLL {};
GetDpiForMonitorSig fnGetDpiForMonitor = nullptr;

using GetSystemMetricsForDpiSig = int (WINAPI *)(int nIndex, UINT dpi);
GetSystemMetricsForDpiSig fnGetSystemMetricsForDpi = nullptr;

using AdjustWindowRectExForDpiSig = BOOL (WINAPI *)(LPRECT lpRect, DWORD dwStyle, BOOL bMenu, DWORD dwExStyle, UINT dpi);
AdjustWindowRectExForDpiSig fnAdjustWindowRectExForDpi = nullptr;

}

void Scintilla_LoadDpiForWindow(void) {
	using Scintilla::DLLFunction;

	HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
	fnGetDpiForWindow = DLLFunction<GetDpiForWindowSig>(user32, "GetDpiForWindow");
	fnGetSystemMetricsForDpi = DLLFunction<GetSystemMetricsForDpiSig>(user32, "GetSystemMetricsForDpi");
	fnAdjustWindowRectExForDpi = DLLFunction<AdjustWindowRectExForDpiSig>(user32, "AdjustWindowRectExForDpi");

	using GetDpiForSystemSig = UINT (WINAPI *)(void);
	GetDpiForSystemSig fnGetDpiForSystem = DLLFunction<GetDpiForSystemSig>(user32, "GetDpiForSystem");
	if (fnGetDpiForSystem) {
		g_uSystemDPI = fnGetDpiForSystem();
	} else {
		HDC hDC = ::GetDC({});
		g_uSystemDPI = ::GetDeviceCaps(hDC, LOGPIXELSY);
		::ReleaseDC({}, hDC);
	}

	if (!fnGetDpiForWindow) {
		hShcoreDLL = ::LoadLibraryExW(L"shcore.dll", {}, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (hShcoreDLL) {
			fnGetDpiForMonitor = DLLFunction<GetDpiForMonitorSig>(hShcoreDLL, "GetDpiForMonitor");
		}
	}
}

UINT GetWindowDPI(HWND hwnd) NP2F_noexcept {
	if (fnGetDpiForWindow) {
		return fnGetDpiForWindow(hwnd);
	}
	if (fnGetDpiForMonitor) {
		HMONITOR hMonitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
		UINT dpiX = 0;
		UINT dpiY = 0;
		if (fnGetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY) == S_OK) {
			return dpiY;
		}
	}
	return g_uSystemDPI;
}

int SystemMetricsForDpi(int nIndex, UINT dpi) NP2F_noexcept {
	if (fnGetSystemMetricsForDpi) {
		return fnGetSystemMetricsForDpi(nIndex, dpi);
	}

	int value = ::GetSystemMetrics(nIndex);
	value = (dpi == g_uSystemDPI) ? value : ::MulDiv(value, dpi, g_uSystemDPI);
	return value;
}

BOOL AdjustWindowRectForDpi(LPRECT lpRect, DWORD dwStyle, DWORD dwExStyle, UINT dpi) NP2F_noexcept {
	if (fnAdjustWindowRectExForDpi) {
		return fnAdjustWindowRectExForDpi(lpRect, dwStyle, FALSE, dwExStyle, dpi);
	}
	return ::AdjustWindowRectEx(lpRect, dwStyle, FALSE, dwExStyle);
}
#endif

namespace Scintilla {

#if defined(USE_D2D)
IDWriteFactory *pIDWriteFactory = nullptr;
ID2D1Factory *pD2DFactory = nullptr;
IDWriteRenderingParams *defaultRenderingParams = nullptr;
IDWriteRenderingParams *customClearTypeRenderingParams = nullptr;
IDWriteGdiInterop *gdiInterop = nullptr;
D2D1_DRAW_TEXT_OPTIONS d2dDrawTextOptions = D2D1_DRAW_TEXT_OPTIONS_NONE;

static HMODULE hDLLD2D {};
static HMODULE hDLLDWrite {};

extern "C" const GUID __declspec(selectany) IID_IDWriteFactory2 = // 0439fc60-ca44-4994-8dee-3a9af7b732ec
{ 0x0439fc60, 0xca44, 0x4994, { 0x8d, 0xee, 0x3a, 0x9a, 0xf7, 0xb7, 0x32, 0xec } };

#if USE_WIN32_INIT_ONCE
static BOOL CALLBACK LoadD2DOnce([[maybe_unused]] PINIT_ONCE initOnce, [[maybe_unused]] PVOID parameter, [[maybe_unused]] PVOID *lpContext) noexcept
#else
static void LoadD2DOnce() noexcept
#endif
{
	// Availability of SetDefaultDllDirectories implies Windows 8+ or
	// that KB2533623 has been installed so LoadLibraryEx can be called
	// with LOAD_LIBRARY_SEARCH_SYSTEM32.

	using D2D1CreateFactorySig = HRESULT(WINAPI *)(D2D1_FACTORY_TYPE factoryType, REFIID riid,
		CONST D2D1_FACTORY_OPTIONS *pFactoryOptions, IUnknown **factory);
	using DWriteCreateFactorySig = HRESULT(WINAPI *)(DWRITE_FACTORY_TYPE factoryType, REFIID iid,
		IUnknown **factory);

	hDLLD2D = ::LoadLibraryEx(L"d2d1.dll", {}, kSystemLibraryLoadFlags);
	if (hDLLD2D) {
		D2D1CreateFactorySig fnD2DCF = DLLFunction<D2D1CreateFactorySig>(hDLLD2D, "D2D1CreateFactory");
		if (fnD2DCF) {
#ifdef NDEBUG
			// A single threaded factory as Scintilla always draw on the GUI thread
			fnD2DCF(D2D1_FACTORY_TYPE_SINGLE_THREADED,
				__uuidof(ID2D1Factory),
				nullptr,
				reinterpret_cast<IUnknown**>(&pD2DFactory));
#else
			D2D1_FACTORY_OPTIONS options = {};
			options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
			fnD2DCF(D2D1_FACTORY_TYPE_SINGLE_THREADED,
				__uuidof(ID2D1Factory),
				&options,
				reinterpret_cast<IUnknown**>(&pD2DFactory));
#endif
		}
	}

	hDLLDWrite = ::LoadLibraryEx(L"dwrite.dll", {}, kSystemLibraryLoadFlags);
	if (hDLLDWrite) {
		DWriteCreateFactorySig fnDWCF = DLLFunction<DWriteCreateFactorySig>(hDLLDWrite, "DWriteCreateFactory");
		if (fnDWCF) {
			const HRESULT hr = fnDWCF(DWRITE_FACTORY_TYPE_SHARED,
				IID_IDWriteFactory2,
				reinterpret_cast<IUnknown**>(&pIDWriteFactory));
			if (SUCCEEDED(hr)) {
				// D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT
				d2dDrawTextOptions = static_cast<D2D1_DRAW_TEXT_OPTIONS>(0x00000004);
			} else {
				fnDWCF(DWRITE_FACTORY_TYPE_SHARED,
					__uuidof(IDWriteFactory),
					reinterpret_cast<IUnknown**>(&pIDWriteFactory));
			}
		}
	}

	if (pIDWriteFactory) {
		HRESULT hr = pIDWriteFactory->CreateRenderingParams(&defaultRenderingParams);
		if (SUCCEEDED(hr)) {
			unsigned int clearTypeContrast = 0;
			if (::SystemParametersInfo(SPI_GETFONTSMOOTHINGCONTRAST, 0, &clearTypeContrast, 0)) {

				FLOAT gamma;
				if (clearTypeContrast >= 1000 && clearTypeContrast <= 2200)
					gamma = static_cast<FLOAT>(clearTypeContrast) / 1000.0f;
				else
					gamma = defaultRenderingParams->GetGamma();

				pIDWriteFactory->CreateCustomRenderingParams(gamma, defaultRenderingParams->GetEnhancedContrast(), defaultRenderingParams->GetClearTypeLevel(),
					defaultRenderingParams->GetPixelGeometry(), defaultRenderingParams->GetRenderingMode(), &customClearTypeRenderingParams);
			}
		}

		hr = pIDWriteFactory->GetGdiInterop(&gdiInterop);
		if (!SUCCEEDED(hr)) {
			ReleaseUnknown(gdiInterop);
		}
	}

#if USE_WIN32_INIT_ONCE
	return TRUE;
#endif
}

bool LoadD2D() noexcept {
#if USE_STD_CALL_ONCE
	static std::once_flag once;
	std::call_once(once, LoadD2DOnce);
#elif USE_WIN32_INIT_ONCE
	static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
	::InitOnceExecuteOnce(&once, LoadD2DOnce, nullptr, nullptr);
#else
	static LONG once = 0;
	if (::InterlockedCompareExchange(&once, 1, 0) == 0) {
		LoadD2DOnce();
	}
#endif
	return pIDWriteFactory && pD2DFactory;
}
#endif

namespace {

// Both GDI and DirectWrite can produce a HFONT for use in list boxes
struct FontWin final : public Font {
	LOGFONTW lf;
	HFONT hfont{};
#if defined(USE_D2D)
	IDWriteTextFormat *pTextFormat = nullptr;
#endif
	int extraFontFlag;
	FLOAT yAscent = 2.0f;
	FLOAT yDescent = 1.0f;
	FLOAT yInternalLeading = 0.0f;
	FontWin(const LOGFONTW &lf_, HFONT hfont_, int extraFontFlag_) noexcept :
		lf(lf_), hfont(hfont_),
		extraFontFlag(extraFontFlag_) {}
#if defined(USE_D2D)
	FontWin(const LOGFONTW &lf_, IDWriteTextFormat *pTextFormat_,
		int extraFontFlag_,
		FLOAT yAscent_,
		FLOAT yDescent_,
		FLOAT yInternalLeading_) noexcept :
		lf(lf_),
		pTextFormat(pTextFormat_),
		extraFontFlag(extraFontFlag_),
		yAscent(yAscent_),
		yDescent(yDescent_),
		yInternalLeading(yInternalLeading_) {}
#endif
	FontWin(const FontWin &) = delete;
	FontWin(FontWin &&) = delete;
	FontWin &operator=(const FontWin &) = delete;
	FontWin &operator=(FontWin &&) = delete;

	~FontWin() noexcept override {
		if (hfont)
			::DeleteObject(hfont);
#if defined(USE_D2D)
		ReleaseUnknown(pTextFormat);
#endif
	}
	HFONT HFont() const noexcept {
		return ::CreateFontIndirectW(&lf);
	}
};

/* dummy types to minimize differences between official Scintilla.
FontDirectWrite::HFont() will create wild font when font family name
is different from typeface name, e.g. Source Code Pro Semibold.
*/
using FontGDI = FontWin;
using FontDirectWrite = FontWin;

HINSTANCE hinstPlatformRes {};

constexpr int SupportsGDI =
	SC_SUPPORTS_PIXEL_MODIFICATION;

#if defined(USE_D2D)
constexpr D2D1_TEXT_ANTIALIAS_MODE DWriteMapFontQuality(int extraFontFlag) noexcept {
	switch (extraFontFlag & SC_EFF_QUALITY_MASK) {

	case SC_EFF_QUALITY_NON_ANTIALIASED:
		return D2D1_TEXT_ANTIALIAS_MODE_ALIASED;

	case SC_EFF_QUALITY_ANTIALIASED:
		return D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE;

	case SC_EFF_QUALITY_LCD_OPTIMIZED:
		return D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE;

	default:
		return D2D1_TEXT_ANTIALIAS_MODE_DEFAULT;
	}
}

bool GetDWriteFontProperties(const LOGFONTW &lf, std::wstring &wsFamily,
	DWRITE_FONT_WEIGHT &weight, DWRITE_FONT_STYLE &style, DWRITE_FONT_STRETCH &stretch) {
	bool success = false;
	if (gdiInterop) {
		IDWriteFont *font = nullptr;
		HRESULT hr = gdiInterop->CreateFontFromLOGFONT(&lf, &font);
		if (SUCCEEDED(hr)) {
			weight = font->GetWeight();
			style = font->GetStyle();
			stretch = font->GetStretch();

			IDWriteFontFamily *family = nullptr;
			hr = font->GetFontFamily(&family);
			if (SUCCEEDED(hr)) {
				IDWriteLocalizedStrings *names = nullptr;
				hr = family->GetFamilyNames(&names);
				if (SUCCEEDED(hr)) {
					UINT32 index = 0;
					BOOL exists = false;
					names->FindLocaleName(L"en-us", &index, &exists);
					if (!exists) {
						index = 0;
					}

					UINT32 length = 0;
					names->GetStringLength(index, &length);

					wsFamily.resize(length + 1);
					names->GetString(index, wsFamily.data(), length + 1);

					ReleaseUnknown(names);
					success = wsFamily[0] != L'\0';
				}

				ReleaseUnknown(family);
			}

			ReleaseUnknown(font);
		}
	}
	return success;
}
#endif

}

std::shared_ptr<Font> Font::Allocate(const FontParameters &fp) {
	LOGFONTW lf {};
	// The negative is to allow for leading
	lf.lfHeight = -std::abs(std::lround(fp.size));
	lf.lfWeight = fp.weight;
	lf.lfItalic = fp.italic ? 1 : 0;
	lf.lfCharSet = static_cast<BYTE>(fp.characterSet);
	lf.lfQuality = Win32MapFontQuality(fp.extraFontFlag);
	UTF16FromUTF8(fp.faceName, lf.lfFaceName, LF_FACESIZE);
	if (fp.technology == SCWIN_TECH_GDI) {
		HFONT hfont = ::CreateFontIndirectW(&lf);
		return std::make_shared<FontGDI>(lf, hfont, fp.extraFontFlag);
	} else {
#if defined(USE_D2D)
		IDWriteTextFormat *pTextFormat = nullptr;
		std::wstring wsFamily;
		const FLOAT fHeight = static_cast<FLOAT>(fp.size);
		DWRITE_FONT_WEIGHT weight = static_cast<DWRITE_FONT_WEIGHT>(fp.weight);
		DWRITE_FONT_STYLE style = fp.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
		DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;
		if (!GetDWriteFontProperties(lf, wsFamily, weight, style, stretch)) {
			wsFamily = WStringFromUTF8(fp.faceName);
		}

		const std::wstring wsLocale = WStringFromUTF8(fp.localeName);
		HRESULT hr = pIDWriteFactory->CreateTextFormat(wsFamily.c_str(), nullptr,
			weight, style, stretch, fHeight, wsLocale.c_str(), &pTextFormat);
		if (hr == E_INVALIDARG) {
			// Possibly a bad locale name like "/" so try "en-us".
			hr = pIDWriteFactory->CreateTextFormat(wsFamily.c_str(), nullptr,
					weight, style, stretch, fHeight, L"en-us", &pTextFormat);
		}
		if (SUCCEEDED(hr)) {
			pTextFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

			FLOAT yAscent = 2.0f;
			FLOAT yDescent = 1.0f;
			FLOAT yInternalLeading = 0.0f;
			IDWriteTextLayout *pTextLayout = nullptr;
			hr = pIDWriteFactory->CreateTextLayout(L"X", 1, pTextFormat,
				100.0f, 100.0f, &pTextLayout);
			if (SUCCEEDED(hr) && pTextLayout) {
				constexpr int maxLines = 2;
				DWRITE_LINE_METRICS lineMetrics[maxLines]{};
				UINT32 lineCount = 0;
				hr = pTextLayout->GetLineMetrics(lineMetrics, maxLines, &lineCount);
				if (SUCCEEDED(hr)) {
					yAscent = lineMetrics[0].baseline;
					yDescent = lineMetrics[0].height - lineMetrics[0].baseline;

					FLOAT emHeight;
					hr = pTextLayout->GetFontSize(0, &emHeight);
					if (SUCCEEDED(hr)) {
						yInternalLeading = lineMetrics[0].height - emHeight;
					}
				}
				ReleaseUnknown(pTextLayout);
				pTextFormat->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineMetrics[0].height, lineMetrics[0].baseline);
			}
			return std::make_shared<FontDirectWrite>(lf, pTextFormat, fp.extraFontFlag, yAscent, yDescent, yInternalLeading);
		}
#endif
	}
	return {};
}

// Buffer to hold strings and string position arrays without always allocating on heap.
// May sometimes have string too long to allocate on stack. So use a fixed stack-allocated buffer
// when less than safe size otherwise allocate on heap and free automatically.
template<typename T, int lengthStandard>
class VarBuffer {
	T bufferStandard[lengthStandard];
public:
	T * buffer;
	explicit VarBuffer(size_t length) : buffer(nullptr) {
		if (length > lengthStandard) {
			buffer = new T[length];
		} else {
			buffer = bufferStandard;
		}
	}
	// Deleted so VarBuffer objects can not be copied.
	VarBuffer(const VarBuffer &) = delete;
	VarBuffer(VarBuffer &&) = delete;
	VarBuffer &operator=(const VarBuffer &) = delete;
	VarBuffer &operator=(VarBuffer &&) = delete;

	~VarBuffer() noexcept {
		if (buffer != bufferStandard) {
			delete[]buffer;
			buffer = nullptr;
		}
	}
};

// limited to BreakFinder::lengthStartSubdivision for drawing document text in editor window.
// no limit for drawing other text, e.g. auto-completion list, calltip, annotation, etc.
constexpr int stackBufferLength = 300;
class TextWide : public VarBuffer<wchar_t, stackBufferLength> {
public:
	int tlen;	// Using int instead of size_t as most Win32 APIs take int.
	TextWide(std::string_view text, int codePage) :
		VarBuffer<wchar_t, stackBufferLength>(text.length()) {
		if (codePage == SC_CP_UTF8) {
			tlen = static_cast<int>(UTF16FromUTF8(text, buffer, text.length()));
		} else {
			// Support Asian string display in 9x English
			tlen = ::MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.length()),
				buffer, static_cast<int>(text.length()));
		}
	}
};
using TextPositions = VarBuffer<XYPOSITION, stackBufferLength>;

class SurfaceGDI final : public Surface {
	SurfaceMode mode;
	HDC hdc{};
	bool hdcOwned = false;
	HPEN pen{};
	HPEN penOld{};
	HBRUSH brush{};
	HBRUSH brushOld{};
	HFONT fontOld{};
	HBITMAP bitmap{};
	HBITMAP bitmapOld{};

	int logPixelsY = USER_DEFAULT_SCREEN_DPI;

	int maxWidthMeasure = INT_MAX;
	// There appears to be a 16 bit string length limit in GDI on NT.
	int maxLenText = 65535;

	void PenColour(ColourAlpha fore, XYPOSITION widthStroke) noexcept;
	void BrushColour(ColourAlpha back) noexcept;
	void SetFont(const Font *font_) noexcept;
	void Clear() noexcept;

public:
	SurfaceGDI() noexcept = default;
	SurfaceGDI(HDC hdcCompatible, int width, int height, SurfaceMode mode_, int logPixelsY_) noexcept;
	// Deleted so SurfaceGDI objects can not be copied.
	SurfaceGDI(const SurfaceGDI &) = delete;
	SurfaceGDI(SurfaceGDI &&) = delete;
	SurfaceGDI &operator=(const SurfaceGDI &) = delete;
	SurfaceGDI &operator=(SurfaceGDI &&) = delete;

	~SurfaceGDI() noexcept override;

	void Init(WindowID wid) noexcept override;
	void Init(SurfaceID sid, WindowID wid, bool printing = false) noexcept override;
	std::unique_ptr<Surface> AllocatePixMap(int width, int height) override;

	void SetMode(SurfaceMode mode_) noexcept override;

	void Release() noexcept override;
	bool Supports(int feature) const noexcept override;
	bool Initialised() const noexcept override;
	int LogPixelsY() const noexcept override;
	int PixelDivisions() const noexcept override;
	int DeviceHeightFont(int points) const noexcept override;
	void SCICALL LineDraw(Point start, Point end, Stroke stroke) noexcept override;
	void SCICALL PolyLine(const Point *pts, size_t npts, Stroke stroke) override;
	void SCICALL Polygon(const Point *pts, size_t npts, FillStroke fillStroke) override;
	void SCICALL RectangleDraw(PRectangle rc, FillStroke fillStroke) noexcept override;
	void SCICALL RectangleFrame(PRectangle rc, Stroke stroke) noexcept override;
	void SCICALL FillRectangle(PRectangle rc, Fill fill) noexcept override;
	void SCICALL FillRectangleAligned(PRectangle rc, Fill fill) noexcept override;
	void SCICALL FillRectangle(PRectangle rc, Surface &surfacePattern) noexcept override;
	void SCICALL RoundedRectangle(PRectangle rc, FillStroke fillStroke) noexcept override;
	void SCICALL AlphaRectangle(PRectangle rc, XYPOSITION cornerSize, FillStroke fillStroke) noexcept override;
	void SCICALL GradientRectangle(PRectangle rc, const std::vector<ColourStop> &stops, GradientOptions options) override;
	void SCICALL DrawRGBAImage(PRectangle rc, int width, int height, const unsigned char *pixelsImage) noexcept override;
	void SCICALL Ellipse(PRectangle rc, FillStroke fillStroke) noexcept override;
	void SCICALL Stadium(PRectangle rc, FillStroke fillStroke, Ends ends) noexcept override;
	void SCICALL Copy(PRectangle rc, Point from, Surface &surfaceSource) noexcept override;

	std::unique_ptr<IScreenLineLayout> Layout(const IScreenLine *screenLine) noexcept override;

	void SCICALL DrawTextCommon(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text, UINT fuOptions);
	void SCICALL DrawTextNoClip(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text, ColourAlpha fore, ColourAlpha back) override;
	void SCICALL DrawTextClipped(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text, ColourAlpha fore, ColourAlpha back) override;
	void SCICALL DrawTextTransparent(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text, ColourAlpha fore) override;
	void SCICALL MeasureWidths(const Font *font_, std::string_view text, XYPOSITION *positions) override;
	XYPOSITION WidthText(const Font *font_, std::string_view text) override;

	void SCICALL DrawTextCommonUTF8(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text, UINT fuOptions);
	void SCICALL DrawTextNoClipUTF8(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text, ColourAlpha fore, ColourAlpha back) override;
	void SCICALL DrawTextClippedUTF8(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text, ColourAlpha fore, ColourAlpha back) override;
	void SCICALL DrawTextTransparentUTF8(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text, ColourAlpha fore) override;
	void SCICALL MeasureWidthsUTF8(const Font *font_, std::string_view text, XYPOSITION *positions) override;
	XYPOSITION WidthTextUTF8(const Font *font_, std::string_view text) override;

	XYPOSITION Ascent(const Font *font_) noexcept override;
	XYPOSITION Descent(const Font *font_) noexcept override;
	XYPOSITION InternalLeading(const Font *font_) noexcept override;
	XYPOSITION Height(const Font *font_) noexcept override;
	XYPOSITION AverageCharWidth(const Font *font_) noexcept override;

	void SCICALL SetClip(PRectangle rc) noexcept override;
	void PopClip() noexcept override;
	void FlushCachedState() noexcept override;
	void FlushDrawing() noexcept override;
};

SurfaceGDI::SurfaceGDI(HDC hdcCompatible, int width, int height, SurfaceMode mode_, int logPixelsY_) noexcept {
	hdc = ::CreateCompatibleDC(hdc);
	hdcOwned = true;
	bitmap = ::CreateCompatibleBitmap(hdcCompatible, width, height);
	bitmapOld = SelectBitmap(hdc, bitmap);
	::SetTextAlign(hdc, TA_BASELINE);
	mode = mode_;
	logPixelsY = logPixelsY_;
}

SurfaceGDI::~SurfaceGDI() noexcept {
	Clear();
}

void SurfaceGDI::Clear() noexcept {
	if (penOld) {
		::SelectObject(hdc, penOld);
		::DeleteObject(pen);
		penOld = {};
	}
	pen = {};
	if (brushOld) {
		::SelectObject(hdc, brushOld);
		::DeleteObject(brush);
		brushOld = {};
	}
	brush = {};
	if (fontOld) {
		// Fonts are not deleted as they are owned by a Font object
		::SelectObject(hdc, fontOld);
		fontOld = {};
	}
	if (bitmapOld) {
		::SelectObject(hdc, bitmapOld);
		::DeleteObject(bitmap);
		bitmapOld = {};
	}
	bitmap = {};
	if (hdcOwned) {
		::DeleteDC(hdc);
		hdc = {};
		hdcOwned = false;
	}
}

void SurfaceGDI::Release() noexcept {
	Clear();
}

bool SurfaceGDI::Supports(int feature) const noexcept {
	return SupportsGDI == feature;
}

bool SurfaceGDI::Initialised() const noexcept {
	return hdc != nullptr;
}

void SurfaceGDI::Init(WindowID wid) noexcept {
	Release();
	logPixelsY = DpiForWindow(wid);
	hdc = ::CreateCompatibleDC({});
	hdcOwned = true;
	::SetTextAlign(hdc, TA_BASELINE);
}

void SurfaceGDI::Init(SurfaceID sid, WindowID wid, bool printing) noexcept {
	Release();
	hdc = static_cast<HDC>(sid);
	// Windows on screen are scaled but printers are not.
	//const bool printing = (::GetDeviceCaps(hdc, TECHNOLOGY) != DT_RASDISPLAY);
	logPixelsY = printing ? ::GetDeviceCaps(hdc, LOGPIXELSY) : DpiForWindow(wid);
	::SetTextAlign(hdc, TA_BASELINE);
}

std::unique_ptr<Surface> SurfaceGDI::AllocatePixMap(int width, int height) {
	return std::make_unique<SurfaceGDI>(hdc, width, height, mode, logPixelsY);
}

void SurfaceGDI::SetMode(SurfaceMode mode_) noexcept {
	mode = mode_;
}

void SurfaceGDI::PenColour(ColourAlpha fore, XYPOSITION widthStroke) noexcept {
	if (pen) {
		::SelectObject(hdc, penOld);
		::DeleteObject(pen);
		pen = {};
		penOld = {};
	}
	const DWORD penWidth = std::lround(widthStroke);
	const COLORREF penColour = fore.OpaqueRGB();
	if (widthStroke > 1) {
		const LOGBRUSH brushParameters { BS_SOLID, penColour, 0 };
		pen = ::ExtCreatePen(PS_GEOMETRIC | PS_ENDCAP_ROUND | PS_JOIN_MITER,
			penWidth,
			&brushParameters,
			0,
			nullptr);
	} else {
		pen = ::CreatePen(PS_INSIDEFRAME, penWidth, penColour);
	}
	penOld = SelectPen(hdc, pen);
}

void SurfaceGDI::BrushColour(ColourAlpha back) noexcept {
	if (brush) {
		::SelectObject(hdc, brushOld);
		::DeleteObject(brush);
		brush = {};
		brushOld = {};
	}
	brush = ::CreateSolidBrush(back.OpaqueRGB());
	brushOld = SelectBrush(hdc, brush);
}

void SurfaceGDI::SetFont(const Font *font_) noexcept {
	const FontGDI *pfm = down_cast<const FontGDI *>(font_);
	PLATFORM_ASSERT(pfm);
	if (fontOld) {
		SelectFont(hdc, pfm->hfont);
	} else {
		fontOld = SelectFont(hdc, pfm->hfont);
	}
}

int SurfaceGDI::LogPixelsY() const noexcept {
	return logPixelsY;
}

int SurfaceGDI::PixelDivisions() const noexcept {
	// Win32 uses device pixels.
	return 1;
}

int SurfaceGDI::DeviceHeightFont(int points) const noexcept {
	return ::MulDiv(points, logPixelsY, 72);
}

void SurfaceGDI::LineDraw(Point start, Point end, Stroke stroke) noexcept {
	PenColour(stroke.colour, stroke.width);
	::MoveToEx(hdc, static_cast<int>(start.x), static_cast<int>(start.y), nullptr);
	::LineTo(hdc, static_cast<int>(end.x), static_cast<int>(end.y));
}

void SurfaceGDI::PolyLine(const Point *pts, size_t npts, Stroke stroke) {
	PLATFORM_ASSERT(npts > 1);
	if (npts <= 1) {
		return;
	}
	PenColour(stroke.colour, stroke.width);
	std::vector<POINT> outline;
	std::transform(pts, pts + npts, std::back_inserter(outline), POINTFromPointEx);
	::Polyline(hdc, outline.data(), static_cast<int>(npts));
}

void SurfaceGDI::Polygon(const Point *pts, size_t npts, FillStroke fillStroke) {
	PenColour(fillStroke.stroke.colour.WithoutAlpha(), fillStroke.stroke.width);
	BrushColour(fillStroke.fill.colour.WithoutAlpha());
	std::vector<POINT> outline;
	std::transform(pts, pts + npts, std::back_inserter(outline), POINTFromPointEx);
	::Polygon(hdc, outline.data(), static_cast<int>(npts));
}

void SurfaceGDI::RectangleDraw(PRectangle rc, FillStroke fillStroke) noexcept {
	RectangleFrame(rc, fillStroke.stroke);
	FillRectangle(rc.Inset(fillStroke.stroke.width), fillStroke.fill.colour);
}

void SurfaceGDI::RectangleFrame(PRectangle rc, Stroke stroke) noexcept {
	BrushColour(stroke.colour);
	const RECT rcw = RectFromPRectangleEx(rc);
	::FrameRect(hdc, &rcw, brush);
}

void SurfaceGDI::FillRectangle(PRectangle rc, Fill fill) noexcept {
	if (fill.colour.IsOpaque()) {
		// Using ExtTextOut rather than a FillRect ensures that no dithering occurs.
		// There is no need to allocate a brush either.
		const RECT rcw = RectFromPRectangleEx(rc);
		::SetBkColor(hdc, fill.colour.OpaqueRGB());
		::ExtTextOut(hdc, rcw.left, rcw.top, ETO_OPAQUE, &rcw, TEXT(""), 0, nullptr);
	} else {
		AlphaRectangle(rc, 0, FillStroke(fill.colour));
	}
}

void SurfaceGDI::FillRectangleAligned(PRectangle rc, Fill fill) noexcept {
	FillRectangle(PixelAlign(rc, 1), fill);
}

void SurfaceGDI::FillRectangle(PRectangle rc, Surface &surfacePattern) noexcept {
	HBRUSH br;
	if (SurfaceGDI *psgdi = down_cast<SurfaceGDI *>(&surfacePattern); psgdi && psgdi->bitmap) {
		br = ::CreatePatternBrush(psgdi->bitmap);
	} else {	// Something is wrong so display in red
		br = ::CreateSolidBrush(RGB(0xff, 0, 0));
	}
	const RECT rcw = RectFromPRectangleEx(rc);
	::FillRect(hdc, &rcw, br);
	::DeleteObject(br);
}

void SurfaceGDI::RoundedRectangle(PRectangle rc, FillStroke fillStroke) noexcept {
	PenColour(fillStroke.stroke.colour, fillStroke.stroke.width);
	BrushColour(fillStroke.fill.colour);
	const RECT rcw = RectFromPRectangleEx(rc);
	::RoundRect(hdc,
		rcw.left + 1, rcw.top,
		rcw.right - 1, rcw.bottom,
		8, 8);
}

namespace {

#if NP2_USE_AVX2
inline DWORD RGBQuadMultiplied(ColourAlpha colour) noexcept {
	__m128i i16x4Color = rgba_to_bgra_epi16_sse4_si32(colour.AsInteger());
	__m128i i16x4Alpha = _mm_shufflelo_epi16(i16x4Color, 0xff);
	i16x4Color = _mm_mullo_epi16(i16x4Color, i16x4Alpha);
	i16x4Color = mm_div_epu16_by_255(i16x4Color);
	i16x4Color = _mm_blend_epi16(i16x4Alpha, i16x4Color, 7);
	return pack_color_epi16_sse2_si32(i16x4Color);
}

#elif NP2_USE_SSE2
inline DWORD RGBQuadMultiplied(ColourAlpha colour) noexcept {
	const uint32_t rgba = bswap32(colour.AsInteger());
	__m128i i16x4Color = unpack_color_epi16_sse2_si32(rgba);
	__m128i i16x4Alpha = _mm_shufflelo_epi16(i16x4Color, 0);
	i16x4Color = _mm_mullo_epi16(i16x4Color, i16x4Alpha);
	i16x4Color = mm_div_epu16_by_255(i16x4Color);

	const uint32_t color = bgr_from_abgr_epi16_sse2_si32(i16x4Color);
	return color | (rgba << 24);
}

#else
// make a GDI RGBQUAD as DWORD.
template <typename T>
constexpr DWORD RGBQuad(T alpha, T red, T green, T blue) noexcept {
	return (alpha << 24) | (red << 16) | (green << 8) | blue;
}

template <typename T>
constexpr T AlphaScaled(T component, T alpha) noexcept {
	return static_cast<T>(component * alpha / 255);
}

template <typename T>
constexpr DWORD RGBQuadMultiplied(T alpha, T red, T green, T blue) noexcept {
	red = AlphaScaled(red, alpha);
	green = AlphaScaled(green, alpha);
	blue = AlphaScaled(blue, alpha);
	return RGBQuad(alpha, red, green, blue);
}

constexpr DWORD RGBQuadMultiplied(ColourAlpha colour) noexcept {
	return RGBQuadMultiplied(colour.GetAlpha(), colour.GetRed(), colour.GetGreen(), colour.GetBlue());
}
#endif

class DIBSection {
	HDC hMemDC{};
	HBITMAP hbmMem{};
	HBITMAP hbmOld{};
	SIZE size{};
	DWORD *pixels = nullptr;
public:
	DIBSection(HDC hdc, SIZE size_) noexcept;
	// Deleted so DIBSection objects can not be copied.
	DIBSection(const DIBSection&) = delete;
	DIBSection(DIBSection&&) = delete;
	DIBSection &operator=(const DIBSection&) = delete;
	DIBSection &operator=(DIBSection&&) = delete;
	~DIBSection() noexcept;
	operator bool() const noexcept {
		return hMemDC && hbmMem && pixels;
	}
	DWORD *Pixels() const noexcept {
		return pixels;
	}
	unsigned char *Bytes() const noexcept {
		return reinterpret_cast<unsigned char *>(pixels);
	}
	HDC DC() const noexcept {
		return hMemDC;
	}
	void SetPixel(LONG x, LONG y, DWORD value) noexcept {
		PLATFORM_ASSERT(x >= 0);
		PLATFORM_ASSERT(y >= 0);
		PLATFORM_ASSERT(x < size.cx);
		PLATFORM_ASSERT(y < size.cy);
		pixels[y * size.cx + x] = value;
	}
	void SetSymmetric(LONG x, LONG y, DWORD value) noexcept;
};

DIBSection::DIBSection(HDC hdc, SIZE size_) noexcept {
	hMemDC = ::CreateCompatibleDC(hdc);
	if (!hMemDC) {
		return;
	}

	size = size_;

	// -size.y makes bitmap start from top
	const BITMAPINFO bpih = { {sizeof(BITMAPINFOHEADER), size.cx, -size.cy, 1, 32, BI_RGB, 0, 0, 0, 0, 0},
		{{0, 0, 0, 0}} };
	void *image = nullptr;
	hbmMem = CreateDIBSection(hMemDC, &bpih, DIB_RGB_COLORS, &image, {}, 0);
	if (!hbmMem || !image) {
		return;
	}
	pixels = static_cast<DWORD *>(image);
	hbmOld = SelectBitmap(hMemDC, hbmMem);
}

DIBSection::~DIBSection() noexcept {
	if (hbmOld) {
		SelectBitmap(hMemDC, hbmOld);
		hbmOld = {};
	}
	if (hbmMem) {
		::DeleteObject(hbmMem);
		hbmMem = {};
	}
	if (hMemDC) {
		::DeleteDC(hMemDC);
		hMemDC = {};
	}
}

void DIBSection::SetSymmetric(LONG x, LONG y, DWORD value) noexcept {
	// Plot a point symmetrically to all 4 quadrants
	const LONG xSymmetric = size.cx - 1 - x;
	const LONG ySymmetric = size.cy - 1 - y;
	SetPixel(x, y, value);
	SetPixel(xSymmetric, y, value);
	SetPixel(x, ySymmetric, value);
	SetPixel(xSymmetric, ySymmetric, value);
}

#if NP2_USE_AVX2
inline DWORD Proportional(ColourAlpha a, ColourAlpha b, XYPOSITION t) noexcept {
	__m128i i32x4Fore = rgba_to_abgr_epi32_sse4_si32(a.AsInteger());
	__m128i i32x4Back = rgba_to_abgr_epi32_sse4_si32(b.AsInteger());
	// a + t * (b - a)
	__m128 f32x4Fore = _mm_cvtepi32_ps(_mm_sub_epi32(i32x4Back, i32x4Fore));
	f32x4Fore = _mm_mul_ps(f32x4Fore, _mm_set1_ps((float)t));
	f32x4Fore = _mm_add_ps(f32x4Fore, _mm_cvtepi32_ps(i32x4Fore));
	// component * alpha / 255
	__m128 f32x4Alpha = _mm_broadcastss_ps(f32x4Fore);
	f32x4Fore = _mm_mul_ps(f32x4Fore, f32x4Alpha);
	f32x4Fore = _mm_div_ps(f32x4Fore, _mm_set1_ps(255.0f));
	f32x4Fore = mm_alignr_ps(f32x4Alpha, f32x4Fore, 1);

	i32x4Fore = _mm_cvttps_epi32(f32x4Fore);
	return pack_color_epi32_sse2_si32(i32x4Fore);
}

#elif NP2_USE_SSE2
inline DWORD Proportional(ColourAlpha a, ColourAlpha b, XYPOSITION t) noexcept {
	__m128i i32x4Fore = rgba_to_abgr_epi32_sse2_si32(a.AsInteger());
	__m128i i32x4Back = rgba_to_abgr_epi32_sse2_si32(b.AsInteger());
	// a + t * (b - a)
	__m128 f32x4Fore = _mm_cvtepi32_ps(_mm_sub_epi32(i32x4Back, i32x4Fore));
	f32x4Fore = _mm_mul_ps(f32x4Fore, _mm_set1_ps((float)t));
	f32x4Fore = _mm_add_ps(f32x4Fore, _mm_cvtepi32_ps(i32x4Fore));
	// component * alpha / 255
	const uint32_t alpha = _mm_cvttss_si32(f32x4Fore);
	__m128 f32x4Alpha = _mm_shuffle_ps(f32x4Fore, f32x4Fore, 0);
	f32x4Fore = _mm_mul_ps(f32x4Fore, f32x4Alpha);
	f32x4Fore = _mm_div_ps(f32x4Fore, _mm_set1_ps(255.0f));

	i32x4Fore = _mm_cvttps_epi32(f32x4Fore);
	const uint32_t color = bgr_from_abgr_epi32_sse2_si32(i32x4Fore);
	return color | (alpha << 24);
}

#else
constexpr uint32_t Proportional(uint8_t a, uint8_t b, XYPOSITION t) noexcept {
	return static_cast<uint32_t>(a + t * (b - a));
}

constexpr DWORD Proportional(ColourAlpha a, ColourAlpha b, XYPOSITION t) noexcept {
	const uint32_t red = Proportional(a.GetRed(), b.GetRed(), t);
	const uint32_t green = Proportional(a.GetGreen(), b.GetGreen(), t);
	const uint32_t blue = Proportional(a.GetBlue(), b.GetBlue(), t);
	const uint32_t alpha = Proportional(a.GetAlpha(), b.GetAlpha(), t);
	return RGBQuadMultiplied(alpha, red, green, blue);
}
#endif

DWORD GradientValue(const std::vector<ColourStop> &stops, XYPOSITION proportion) noexcept {
	for (size_t stop = 0; stop < stops.size() - 1; stop++) {
		// Loop through each pair of stops
		const XYPOSITION positionStart = stops[stop].position;
		const XYPOSITION positionEnd = stops[stop + 1].position;
		if ((proportion >= positionStart) && (proportion <= positionEnd)) {
			const XYPOSITION proportionInPair = (proportion - positionStart) / (positionEnd - positionStart);
			return Proportional(stops[stop].colour, stops[stop + 1].colour, proportionInPair);
		}
	}
	// Loop should always find a value
	return 0;
}

constexpr SIZE SizeOfRect(RECT rc) noexcept {
	return { rc.right - rc.left, rc.bottom - rc.top };
}

constexpr const BLENDFUNCTION mergeAlpha = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

}

void SurfaceGDI::AlphaRectangle(PRectangle rc, XYPOSITION cornerSize, FillStroke fillStroke) noexcept {
	// TODO: Implement strokeWidth
	const RECT rcw = RectFromPRectangle(rc);
	const SIZE size = SizeOfRect(rcw);

	if (size.cx > 0) {
		DIBSection section(hdc, size);
		if (section) {
			// Ensure not distorted too much by corners when small
			const LONG corner = std::min(static_cast<LONG>(cornerSize), (std::min(size.cx, size.cy) / 2) - 2);

			constexpr DWORD valEmpty = 0;
			const DWORD valFill = RGBQuadMultiplied(fillStroke.fill.colour);
			const DWORD valOutline = RGBQuadMultiplied(fillStroke.stroke.colour);

			// Draw a framed rectangle
			for (int y = 0; y < size.cy; y++) {
				for (int x = 0; x < size.cx; x++) {
					if ((x == 0) || (x == size.cx - 1) || (y == 0) || (y == size.cy - 1)) {
						section.SetPixel(x, y, valOutline);
					} else {
						section.SetPixel(x, y, valFill);
					}
				}
			}

			// Make the corners transparent
			for (LONG c = 0; c < corner; c++) {
				for (LONG x = 0; x < c + 1; x++) {
					section.SetSymmetric(x, c - x, valEmpty);
				}
			}

			// Draw the corner frame pieces
			for (LONG x = 1; x < corner; x++) {
				section.SetSymmetric(x, corner - x, valOutline);
			}

			GdiAlphaBlend(hdc, rcw.left, rcw.top, size.cx, size.cy, section.DC(), 0, 0, size.cx, size.cy, mergeAlpha);
		}
	} else {
		BrushColour(fillStroke.stroke.colour);
		::FrameRect(hdc, &rcw, brush);
	}
}

void SurfaceGDI::GradientRectangle(PRectangle rc, const std::vector<ColourStop> &stops, GradientOptions options) {
	const RECT rcw = RectFromPRectangle(rc);
	const SIZE size = SizeOfRect(rcw);

	DIBSection section(hdc, size);
	if (section) {
		if (options == GradientOptions::topToBottom) {
			for (LONG y = 0; y < size.cy; y++) {
				// Find y/height proportional colour
				const XYPOSITION proportion = y / (rc.Height() - 1.0f);
				const DWORD valFill = GradientValue(stops, proportion);
				for (LONG x = 0; x < size.cx; x++) {
					section.SetPixel(x, y, valFill);
				}
			}
		} else {
			for (LONG x = 0; x < size.cx; x++) {
				// Find x/width proportional colour
				const XYPOSITION proportion = x / (rc.Width() - 1.0f);
				const DWORD valFill = GradientValue(stops, proportion);
				for (LONG y = 0; y < size.cy; y++) {
					section.SetPixel(x, y, valFill);
				}
			}
		}

		GdiAlphaBlend(hdc, rcw.left, rcw.top, size.cx, size.cy, section.DC(), 0, 0, size.cx, size.cy, mergeAlpha);
	}
}

void SurfaceGDI::DrawRGBAImage(PRectangle rc, int width, int height, const unsigned char *pixelsImage) noexcept {
	if (rc.Width() > 0) {
		if (rc.Width() > width)
			rc.left += std::floor((rc.Width() - width) / 2);
		rc.right = rc.left + width;
		if (rc.Height() > height)
			rc.top += std::floor((rc.Height() - height) / 2);
		rc.bottom = rc.top + height;

		const SIZE size{ width, height };
		DIBSection section(hdc, size);
		if (section) {
			RGBAImage::BGRAFromRGBA(section.Bytes(), pixelsImage, width * height);
			GdiAlphaBlend(hdc, static_cast<int>(rc.left), static_cast<int>(rc.top),
				static_cast<int>(rc.Width()), static_cast<int>(rc.Height()), section.DC(),
				0, 0, width, height, mergeAlpha);
		}
	}
}

void SurfaceGDI::Ellipse(PRectangle rc, FillStroke fillStroke) noexcept {
	PenColour(fillStroke.stroke.colour, fillStroke.stroke.width);
	BrushColour(fillStroke.fill.colour);
	const RECT rcw = RectFromPRectangleEx(rc);
	::Ellipse(hdc, rcw.left, rcw.top, rcw.right, rcw.bottom);
}

void SurfaceGDI::Stadium(PRectangle rc, FillStroke fillStroke, [[maybe_unused]] Ends ends) noexcept {
	// TODO: Implement properly - the rectangle is just a placeholder
	RectangleDraw(rc, fillStroke);
}

void SurfaceGDI::Copy(PRectangle rc, Point from, Surface &surfaceSource) noexcept {
	::BitBlt(hdc,
		static_cast<int>(rc.left), static_cast<int>(rc.top),
		static_cast<int>(rc.Width()), static_cast<int>(rc.Height()),
		down_cast<SurfaceGDI &>(surfaceSource).hdc,
		static_cast<int>(from.x), static_cast<int>(from.y), SRCCOPY);
}

std::unique_ptr<IScreenLineLayout> SurfaceGDI::Layout(const IScreenLine *) noexcept {
	return {};
}

using TextPositionsI = VarBuffer<int, stackBufferLength>;

void SurfaceGDI::DrawTextCommon(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text, UINT fuOptions) {
	SetFont(font_);
	const RECT rcw = RectFromPRectangleEx(rc);
	const int x = static_cast<int>(rcw.left);
	const int yBaseInt = static_cast<int>(ybase);

	if (mode.codePage == SC_CP_UTF8) {
		const TextWide tbuf(text, SC_CP_UTF8);
		::ExtTextOutW(hdc, x, yBaseInt, fuOptions, &rcw, tbuf.buffer, tbuf.tlen, nullptr);
	} else {
		::ExtTextOutA(hdc, x, yBaseInt, fuOptions, &rcw, text.data(), static_cast<UINT>(text.length()), nullptr);
	}
}

void SurfaceGDI::DrawTextNoClip(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text,
	ColourAlpha fore, ColourAlpha back) {
	::SetTextColor(hdc, fore.OpaqueRGB());
	::SetBkColor(hdc, back.OpaqueRGB());
	DrawTextCommon(rc, font_, ybase, text, ETO_OPAQUE);
}

void SurfaceGDI::DrawTextClipped(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text,
	ColourAlpha fore, ColourAlpha back) {
	::SetTextColor(hdc, fore.OpaqueRGB());
	::SetBkColor(hdc, back.OpaqueRGB());
	DrawTextCommon(rc, font_, ybase, text, ETO_OPAQUE | ETO_CLIPPED);
}

void SurfaceGDI::DrawTextTransparent(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text,
	ColourAlpha fore) {
	// Avoid drawing spaces in transparent mode
	for (const char ch : text) {
		if (ch != ' ') {
			::SetTextColor(hdc, fore.OpaqueRGB());
			::SetBkMode(hdc, TRANSPARENT);
			DrawTextCommon(rc, font_, ybase, text, 0);
			::SetBkMode(hdc, OPAQUE);
			return;
		}
	}
}

void SurfaceGDI::MeasureWidths(const Font *font_, std::string_view text, XYPOSITION *positions) {
	// Zero positions to avoid random behaviour on failure.
	std::fill(positions, positions + text.length(), 0.0f);
	SetFont(font_);
	SIZE sz {};
	int fit = 0;
	int i = 0;
	const int len = static_cast<int>(text.length());
	if (mode.codePage == SC_CP_UTF8) {
		const TextWide tbuf(text, SC_CP_UTF8);
		TextPositionsI poses(tbuf.tlen);
		if (!::GetTextExtentExPointW(hdc, tbuf.buffer, tbuf.tlen, maxWidthMeasure, &fit, poses.buffer, &sz)) {
			// Failure
			return;
		}
		// Map the widths given for UTF-16 characters back onto the UTF-8 input string
		for (int ui = 0; ui < fit; ui++) {
			const unsigned char uch = text[i];
			const unsigned int byteCount = UTF8BytesOfLead(uch);
			if (byteCount == 4) {	// Non-BMP
				ui++;
			}
			const XYPOSITION pos = static_cast<XYPOSITION>(poses.buffer[ui]);
			for (unsigned int bytePos = 0; (bytePos < byteCount) && (i < len); bytePos++) {
				positions[i++] = pos;
			}
		}
	} else {
		TextPositionsI poses(len);
		if (!::GetTextExtentExPointA(hdc, text.data(), len, maxWidthMeasure, &fit, poses.buffer, &sz)) {
			// Eeek - a NULL DC or other foolishness could cause this.
			return;
		}
		while (i < fit) {
			positions[i] = static_cast<XYPOSITION>(poses.buffer[i]);
			i++;
		}
	}
	// If any positions not filled in then use the last position for them
	const XYPOSITION lastPos = (fit > 0) ? positions[fit - 1] : 0.0f;
	std::fill(positions + i, positions + text.length(), lastPos);
}

XYPOSITION SurfaceGDI::WidthText(const Font *font_, std::string_view text) {
	SetFont(font_);
	SIZE sz {};
	if (mode.codePage != SC_CP_UTF8) {
		::GetTextExtentPoint32A(hdc, text.data(), std::min(static_cast<int>(text.length()), maxLenText), &sz);
	} else {
		const TextWide tbuf(text, SC_CP_UTF8);
		::GetTextExtentPoint32W(hdc, tbuf.buffer, tbuf.tlen, &sz);
	}
	return static_cast<XYPOSITION>(sz.cx);
}

void SurfaceGDI::DrawTextCommonUTF8(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text, UINT fuOptions) {
	SetFont(font_);
	const RECT rcw = RectFromPRectangleEx(rc);
	const int x = static_cast<int>(rcw.left);
	const int yBaseInt = static_cast<int>(ybase);

	const TextWide tbuf(text, SC_CP_UTF8);
	::ExtTextOutW(hdc, x, yBaseInt, fuOptions, &rcw, tbuf.buffer, tbuf.tlen, nullptr);
}

void SurfaceGDI::DrawTextNoClipUTF8(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text,
	ColourAlpha fore, ColourAlpha back) {
	::SetTextColor(hdc, fore.OpaqueRGB());
	::SetBkColor(hdc, back.OpaqueRGB());
	DrawTextCommonUTF8(rc, font_, ybase, text, ETO_OPAQUE);
}

void SurfaceGDI::DrawTextClippedUTF8(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text,
	ColourAlpha fore, ColourAlpha back) {
	::SetTextColor(hdc, fore.OpaqueRGB());
	::SetBkColor(hdc, back.OpaqueRGB());
	DrawTextCommonUTF8(rc, font_, ybase, text, ETO_OPAQUE | ETO_CLIPPED);
}

void SurfaceGDI::DrawTextTransparentUTF8(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text,
	ColourAlpha fore) {
	// Avoid drawing spaces in transparent mode
	for (const char ch : text) {
		if (ch != ' ') {
			::SetTextColor(hdc, fore.OpaqueRGB());
			::SetBkMode(hdc, TRANSPARENT);
			DrawTextCommonUTF8(rc, font_, ybase, text, 0);
			::SetBkMode(hdc, OPAQUE);
			return;
		}
	}
}

void SurfaceGDI::MeasureWidthsUTF8(const Font *font_, std::string_view text, XYPOSITION *positions) {
	// Zero positions to avoid random behaviour on failure.
	std::fill(positions, positions + text.length(), 0.0f);
	SetFont(font_);
	SIZE sz = { 0,0 };
	int fit = 0;
	int i = 0;
	const int len = static_cast<int>(text.length());
	const TextWide tbuf(text, SC_CP_UTF8);
	TextPositionsI poses(tbuf.tlen);
	if (!::GetTextExtentExPointW(hdc, tbuf.buffer, tbuf.tlen, maxWidthMeasure, &fit, poses.buffer, &sz)) {
		// Failure
		return;
	}
	// Map the widths given for UTF-16 characters back onto the UTF-8 input string
	for (int ui = 0; ui < fit; ui++) {
		const unsigned char uch = text[i];
		const unsigned int byteCount = UTF8BytesOfLead(uch);
		if (byteCount == 4) {	// Non-BMP
			ui++;
		}
		for (unsigned int bytePos = 0; (bytePos < byteCount) && (i < len); bytePos++) {
			positions[i++] = static_cast<XYPOSITION>(poses.buffer[ui]);
		}
	}
	// If any positions not filled in then use the last position for them
	const XYPOSITION lastPos = (fit > 0) ? positions[fit - 1] : 0.0f;
	std::fill(positions + i, positions + text.length(), lastPos);
}

XYPOSITION SurfaceGDI::WidthTextUTF8(const Font *font_, std::string_view text) {
	SetFont(font_);
	SIZE sz = { 0,0 };
	const TextWide tbuf(text, SC_CP_UTF8);
	::GetTextExtentPoint32W(hdc, tbuf.buffer, tbuf.tlen, &sz);
	return static_cast<XYPOSITION>(sz.cx);
}

XYPOSITION SurfaceGDI::Ascent(const Font *font_) noexcept {
	SetFont(font_);
	TEXTMETRIC tm;
	::GetTextMetrics(hdc, &tm);
	return static_cast<XYPOSITION>(tm.tmAscent);
}

XYPOSITION SurfaceGDI::Descent(const Font *font_) noexcept {
	SetFont(font_);
	TEXTMETRIC tm;
	::GetTextMetrics(hdc, &tm);
	return static_cast<XYPOSITION>(tm.tmDescent);
}

XYPOSITION SurfaceGDI::InternalLeading(const Font *font_) noexcept {
	SetFont(font_);
	TEXTMETRIC tm;
	::GetTextMetrics(hdc, &tm);
	return static_cast<XYPOSITION>(tm.tmInternalLeading);
}

XYPOSITION SurfaceGDI::Height(const Font *font_) noexcept {
	SetFont(font_);
	TEXTMETRIC tm;
	::GetTextMetrics(hdc, &tm);
	return static_cast<XYPOSITION>(tm.tmHeight);
}

XYPOSITION SurfaceGDI::AverageCharWidth(const Font *font_) noexcept {
	SetFont(font_);
	TEXTMETRIC tm;
	::GetTextMetrics(hdc, &tm);
	return static_cast<XYPOSITION>(tm.tmAveCharWidth);
}

void SurfaceGDI::SetClip(PRectangle rc) noexcept {
	::SaveDC(hdc);
	::IntersectClipRect(hdc, static_cast<int>(rc.left), static_cast<int>(rc.top),
		static_cast<int>(rc.right), static_cast<int>(rc.bottom));
}

void SurfaceGDI::PopClip() noexcept {
	::RestoreDC(hdc, -1);
}

void SurfaceGDI::FlushCachedState() noexcept {
	pen = {};
	brush = {};
}

void SurfaceGDI::FlushDrawing() noexcept {
}

#if defined(USE_D2D)

namespace {

constexpr D2D1_RECT_F RectangleFromPRectangle(PRectangle rc) noexcept {
	return {
		static_cast<FLOAT>(rc.left),
		static_cast<FLOAT>(rc.top),
		static_cast<FLOAT>(rc.right),
		static_cast<FLOAT>(rc.bottom)
	};
}

#if NP2_USE_AVX2
static_assert(sizeof(PRectangle) == sizeof(__m256d));
static_assert(sizeof(D2D1_RECT_F) == sizeof(__m128));

inline D2D1_RECT_F RectangleFromPRectangleEx(PRectangle prc) noexcept {
	D2D1_RECT_F rc;
	__m256d f64x4 = _mm256_load_pd((double *)(&prc));
	__m128 f32x4 = _mm256_cvtpd_ps(f64x4);
	_mm_storeu_ps((float *)(&rc), f32x4);
	return rc;
}

#else
constexpr D2D1_RECT_F RectangleFromPRectangleEx(PRectangle prc) noexcept {
	return RectangleFromPRectangle(prc);
}
#endif

constexpr D2D1_POINT_2F DPointFromPoint(Point point) noexcept {
	return { static_cast<FLOAT>(point.x), static_cast<FLOAT>(point.y) };
}

#if NP2_USE_SSE2
static_assert(sizeof(Point) == sizeof(__m128d));
static_assert(sizeof(D2D1_POINT_2F) == sizeof(__m64));

inline D2D1_POINT_2F DPointFromPointEx(Point point) noexcept {
	D2D1_POINT_2F pt;
	__m128d f64x2 = _mm_load_pd((double *)(&point));
	__m128 f32x2 = _mm_cvtpd_ps(f64x2);
	_mm_storel_pi((__m64 *)(&pt), f32x2);
	return pt;
}

#else
constexpr D2D1_POINT_2F DPointFromPointEx(Point point) noexcept {
	return DPointFromPoint(point);
}
#endif

constexpr unsigned int SupportsD2D =
	(1 << SC_SUPPORTS_LINE_DRAWS_FINAL) |
	(1 << SC_SUPPORTS_FRACTIONAL_STROKE_WIDTH) |
	(1 << SC_SUPPORTS_TRANSLUCENT_STROKE) |
	(1 << SC_SUPPORTS_PIXEL_MODIFICATION);

#if NP2_USE_SSE2
static_assert(sizeof(D2D_COLOR_F) == sizeof(__m128));

inline D2D_COLOR_F ColorFromColourAlpha(ColourAlpha colour) noexcept {
#if NP2_USE_AVX2
	__m128i i32x4 = unpack_color_epi32_sse4_si32(colour.AsInteger());
#else
	__m128i i32x4 = unpack_color_epi32_sse2_si32(colour.AsInteger());
#endif
	__m128 f32x4 = _mm_cvtepi32_ps(i32x4);
	f32x4 = _mm_div_ps(f32x4, _mm_set1_ps(255.0f));
	D2D_COLOR_F color;
	_mm_storeu_ps((float *)&color, f32x4);
	return color;
}

#else
constexpr D2D_COLOR_F ColorFromColourAlpha(ColourAlpha colour) noexcept {
	return D2D_COLOR_F {
		colour.GetRedComponent(),
		colour.GetGreenComponent(),
		colour.GetBlueComponent(),
		colour.GetAlphaComponent()
	};
}
#endif

constexpr D2D1_RECT_F RectangleInset(D2D1_RECT_F rect, FLOAT inset) noexcept {
	return D2D1_RECT_F {
		rect.left + inset,
		rect.top + inset,
		rect.right - inset,
		rect.bottom - inset
	};
}

}

class BlobInline;

class SurfaceD2D final : public Surface {
	SurfaceMode mode;

	ID2D1RenderTarget *pRenderTarget = nullptr;
	ID2D1BitmapRenderTarget *pBitmapRenderTarget = nullptr;
	bool ownRenderTarget = false;
	int clipsActive = 0;

	IDWriteTextFormat *pTextFormat = nullptr;
	FLOAT yAscent = 2;
	FLOAT yDescent = 1;
	FLOAT yInternalLeading = 0;

	ID2D1SolidColorBrush *pBrush = nullptr;

	int logPixelsY = USER_DEFAULT_SCREEN_DPI;

	void Clear() noexcept;
	void SetFont(const Font *font_) noexcept;
	HRESULT GetBitmap(ID2D1Bitmap **ppBitmap);

public:
	SurfaceD2D() noexcept = default;
	SurfaceD2D(ID2D1RenderTarget *pRenderTargetCompatible, int width, int height, SurfaceMode mode_, int logPixelsY_) noexcept;
	// Deleted so SurfaceD2D objects can not be copied.
	SurfaceD2D(const SurfaceD2D &) = delete;
	SurfaceD2D(SurfaceD2D &&) = delete;
	SurfaceD2D &operator=(const SurfaceD2D &) = delete;
	SurfaceD2D &operator=(SurfaceD2D &&) = delete;
	~SurfaceD2D() noexcept override;

	void Init(WindowID wid) noexcept override;
	void Init(SurfaceID sid, WindowID wid, bool printing = false) noexcept override;
	std::unique_ptr<Surface> AllocatePixMap(int width, int height) override;

	void SetMode(SurfaceMode mode_) noexcept override;

	void Release() noexcept override;
	bool Supports(int feature) const noexcept override;
	bool Initialised() const noexcept override;

	void D2DPenColourAlpha(ColourAlpha fore) noexcept;
	int LogPixelsY() const noexcept override;
	int PixelDivisions() const noexcept override;
	int DeviceHeightFont(int points) const noexcept override;
	void SCICALL LineDraw(Point start, Point end, Stroke stroke) override;
	static ID2D1PathGeometry *Geometry(const Point *pts, size_t npts, D2D1_FIGURE_BEGIN figureBegin) noexcept;
	void SCICALL PolyLine(const Point *pts, size_t npts, Stroke stroke) override;
	void SCICALL Polygon(const Point *pts, size_t npts, FillStroke fillStroke) override;
	void SCICALL RectangleDraw(PRectangle rc, FillStroke fillStroke) override;
	void SCICALL RectangleFrame(PRectangle rc, Stroke stroke) override;
	void SCICALL FillRectangle(PRectangle rc, Fill fill) override;
	void SCICALL FillRectangleAligned(PRectangle rc, Fill fill) override;
	void SCICALL FillRectangle(PRectangle rc, Surface &surfacePattern) override;
	void SCICALL RoundedRectangle(PRectangle rc, FillStroke fillStroke) override;
	void SCICALL AlphaRectangle(PRectangle rc, XYPOSITION cornerSize, FillStroke fillStroke) override;
	void SCICALL GradientRectangle(PRectangle rc, const std::vector<ColourStop> &stops, GradientOptions options) override;
	void SCICALL DrawRGBAImage(PRectangle rc, int width, int height, const unsigned char *pixelsImage) override;
	void SCICALL Ellipse(PRectangle rc, FillStroke fillStroke) override;
	void SCICALL Stadium(PRectangle rc, FillStroke fillStroke, Ends ends) override;
	void SCICALL Copy(PRectangle rc, Point from, Surface &surfaceSource) override;

	std::unique_ptr<IScreenLineLayout> Layout(const IScreenLine *screenLine) override;

	void SCICALL DrawTextCommon(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text, int codePageOverride, UINT fuOptions);

	void SCICALL DrawTextNoClip(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text, ColourAlpha fore, ColourAlpha back) override;
	void SCICALL DrawTextClipped(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text, ColourAlpha fore, ColourAlpha back) override;
	void SCICALL DrawTextTransparent(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text, ColourAlpha fore) override;
	void SCICALL MeasureWidths(const Font *font_, std::string_view text, XYPOSITION *positions) override;
	XYPOSITION WidthText(const Font *font_, std::string_view text) override;

	void SCICALL DrawTextNoClipUTF8(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text, ColourAlpha fore, ColourAlpha back) override;
	void SCICALL DrawTextClippedUTF8(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text, ColourAlpha fore, ColourAlpha back) override;
	void SCICALL DrawTextTransparentUTF8(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text, ColourAlpha fore) override;
	void SCICALL MeasureWidthsUTF8(const Font *font_, std::string_view text, XYPOSITION *positions) override;
	XYPOSITION WidthTextUTF8(const Font *font_, std::string_view text) override;

	XYPOSITION Ascent(const Font *font_) noexcept override;
	XYPOSITION Descent(const Font *font_) noexcept override;
	XYPOSITION InternalLeading(const Font *font_) noexcept override;
	XYPOSITION Height(const Font *font_) noexcept override;
	XYPOSITION AverageCharWidth(const Font *font_) override;

	void SCICALL SetClip(PRectangle rc) noexcept override;
	void PopClip() noexcept override;
	void FlushCachedState() noexcept override;

	void FlushDrawing() noexcept override;
};

SurfaceD2D::SurfaceD2D(ID2D1RenderTarget *pRenderTargetCompatible, int width, int height, SurfaceMode mode_, int logPixelsY_) noexcept {
	const D2D1_SIZE_F desiredSize = D2D1::SizeF(static_cast<float>(width), static_cast<float>(height));
	D2D1_PIXEL_FORMAT desiredFormat;
#ifdef __MINGW32__
	desiredFormat.format = DXGI_FORMAT_UNKNOWN;
#else
	desiredFormat = pRenderTargetCompatible->GetPixelFormat();
#endif
	desiredFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
	const HRESULT hr = pRenderTargetCompatible->CreateCompatibleRenderTarget(
		&desiredSize, nullptr, &desiredFormat, D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &pBitmapRenderTarget);
	if (SUCCEEDED(hr)) {
		pRenderTarget = pBitmapRenderTarget;
		pRenderTarget->BeginDraw();
		ownRenderTarget = true;
	}
	mode = mode_;
	logPixelsY = logPixelsY_;
}

SurfaceD2D::~SurfaceD2D() noexcept {
	Clear();
}

void SurfaceD2D::Clear() noexcept {
	ReleaseUnknown(pBrush);
	if (pRenderTarget) {
		while (clipsActive) {
			pRenderTarget->PopAxisAlignedClip();
			clipsActive--;
		}
		if (ownRenderTarget) {
			pRenderTarget->EndDraw();
			ReleaseUnknown(pRenderTarget);
			ownRenderTarget = false;
		}
		pRenderTarget = nullptr;
	}
	pBitmapRenderTarget = nullptr;
}

void SurfaceD2D::Release() noexcept {
	Clear();
}

bool SurfaceD2D::Supports(int feature) const noexcept {
	return (SupportsD2D >> feature) & true;
}

bool SurfaceD2D::Initialised() const noexcept {
	return pRenderTarget != nullptr;
}

void SurfaceD2D::Init(WindowID wid) noexcept {
	Release();
	logPixelsY = DpiForWindow(wid);
}

void SurfaceD2D::Init(SurfaceID sid, WindowID wid, bool /*printing*/) noexcept {
	Release();
	// printing always using GDI
	logPixelsY = DpiForWindow(wid);
	pRenderTarget = static_cast<ID2D1RenderTarget *>(sid);
}

std::unique_ptr<Surface> SurfaceD2D::AllocatePixMap(int width, int height) {
	return std::make_unique<SurfaceD2D>(pRenderTarget, width, height, mode, logPixelsY);
}

void SurfaceD2D::SetMode(SurfaceMode mode_) noexcept {
	mode = mode_;
}

HRESULT SurfaceD2D::GetBitmap(ID2D1Bitmap **ppBitmap) {
	PLATFORM_ASSERT(pBitmapRenderTarget);
	return pBitmapRenderTarget->GetBitmap(ppBitmap);
}

void SurfaceD2D::D2DPenColourAlpha(ColourAlpha fore) noexcept {
	if (pRenderTarget) {
		const D2D_COLOR_F col = ColorFromColourAlpha(fore);
		if (pBrush) {
			pBrush->SetColor(col);
		} else {
			const HRESULT hr = pRenderTarget->CreateSolidColorBrush(col, &pBrush);
			if (!SUCCEEDED(hr)) {
				ReleaseUnknown(pBrush);
			}
		}
	}
}

void SurfaceD2D::SetFont(const Font *font_) noexcept {
	const FontDirectWrite *pfm = down_cast<const FontDirectWrite *>(font_);
	PLATFORM_ASSERT(pfm);
	pTextFormat = pfm->pTextFormat;
	yAscent = pfm->yAscent;
	yDescent = pfm->yDescent;
	yInternalLeading = pfm->yInternalLeading;
	if (pRenderTarget) {
		const D2D1_TEXT_ANTIALIAS_MODE aaMode = DWriteMapFontQuality(pfm->extraFontFlag);

		if (aaMode == D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE && customClearTypeRenderingParams)
			pRenderTarget->SetTextRenderingParams(customClearTypeRenderingParams);
		else if (defaultRenderingParams)
			pRenderTarget->SetTextRenderingParams(defaultRenderingParams);

		pRenderTarget->SetTextAntialiasMode(aaMode);
	}
}

int SurfaceD2D::LogPixelsY() const noexcept {
	return logPixelsY;
}

int SurfaceD2D::PixelDivisions() const noexcept {
	// Win32 uses device pixels.
	return 1;
}

int SurfaceD2D::DeviceHeightFont(int points) const noexcept {
	return ::MulDiv(points, logPixelsY, 72);
}

void SurfaceD2D::LineDraw(Point start, Point end, Stroke stroke) {
	D2DPenColourAlpha(stroke.colour);

	D2D1_STROKE_STYLE_PROPERTIES strokeProps {};
	strokeProps.startCap = D2D1_CAP_STYLE_SQUARE;
	strokeProps.endCap = D2D1_CAP_STYLE_SQUARE;
	strokeProps.dashCap = D2D1_CAP_STYLE_FLAT;
	strokeProps.lineJoin = D2D1_LINE_JOIN_MITER;
	strokeProps.miterLimit = 4.0f;
	strokeProps.dashStyle = D2D1_DASH_STYLE_SOLID;
	strokeProps.dashOffset = 0;

	// get the stroke style to apply
	ID2D1StrokeStyle *pStrokeStyle = nullptr;
	const HRESULT hr = pD2DFactory->CreateStrokeStyle(
		strokeProps, nullptr, 0, &pStrokeStyle);
	if (SUCCEEDED(hr)) {
		pRenderTarget->DrawLine(
			DPointFromPointEx(start),
			DPointFromPointEx(end), pBrush, stroke.WidthF(), pStrokeStyle);
	}

	ReleaseUnknown(pStrokeStyle);
}

ID2D1PathGeometry *SurfaceD2D::Geometry(const Point *pts, size_t npts, D2D1_FIGURE_BEGIN figureBegin) noexcept {
	ID2D1PathGeometry *geometry = nullptr;
	HRESULT hr = pD2DFactory->CreatePathGeometry(&geometry);
	if (SUCCEEDED(hr) && geometry) {
		ID2D1GeometrySink *sink = nullptr;
		hr = geometry->Open(&sink);
		if (SUCCEEDED(hr) && sink) {
			sink->BeginFigure(DPointFromPointEx(pts[0]), figureBegin);
			for (size_t i = 1; i < npts; i++) {
				sink->AddLine(DPointFromPointEx(pts[i]));
			}
			sink->EndFigure((figureBegin == D2D1_FIGURE_BEGIN_FILLED) ?
				D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
			sink->Close();
			ReleaseUnknown(sink);
		}
	}
	return geometry;
}

void SurfaceD2D::PolyLine(const Point *pts, size_t npts, Stroke stroke) {
	PLATFORM_ASSERT(pRenderTarget && (npts > 1));
	if (!pRenderTarget || (npts <= 1)) {
		return;
	}

	ID2D1PathGeometry *geometry = Geometry(pts, npts, D2D1_FIGURE_BEGIN_HOLLOW);
	PLATFORM_ASSERT(geometry);
	if (!geometry) {
		return;
	}

	D2DPenColourAlpha(stroke.colour);
	D2D1_STROKE_STYLE_PROPERTIES strokeProps {};
	strokeProps.startCap = D2D1_CAP_STYLE_ROUND;
	strokeProps.endCap = D2D1_CAP_STYLE_ROUND;
	strokeProps.dashCap = D2D1_CAP_STYLE_FLAT;
	strokeProps.lineJoin = D2D1_LINE_JOIN_MITER;
	strokeProps.miterLimit = 4.0f;
	strokeProps.dashStyle = D2D1_DASH_STYLE_SOLID;
	strokeProps.dashOffset = 0;

	// get the stroke style to apply
	ID2D1StrokeStyle *pStrokeStyle = nullptr;
	const HRESULT hr = pD2DFactory->CreateStrokeStyle(
		strokeProps, nullptr, 0, &pStrokeStyle);
	if (SUCCEEDED(hr)) {
		pRenderTarget->DrawGeometry(geometry, pBrush, stroke.WidthF(), pStrokeStyle);
	}
	ReleaseUnknown(pStrokeStyle);
	ReleaseUnknown(geometry);
}

void SurfaceD2D::Polygon(const Point *pts, size_t npts, FillStroke fillStroke) {
	PLATFORM_ASSERT(pRenderTarget && (npts > 2));
	if (pRenderTarget) {
		ID2D1PathGeometry *geometry = Geometry(pts, npts, D2D1_FIGURE_BEGIN_FILLED);
		PLATFORM_ASSERT(geometry);
		if (geometry) {
			D2DPenColourAlpha(fillStroke.fill.colour);
			pRenderTarget->FillGeometry(geometry, pBrush);
			D2DPenColourAlpha(fillStroke.stroke.colour);
			pRenderTarget->DrawGeometry(geometry, pBrush, fillStroke.stroke.WidthF());
			ReleaseUnknown(geometry);
		}
	}
}

void SurfaceD2D::RectangleDraw(PRectangle rc, FillStroke fillStroke) {
	if (!pRenderTarget)
		return;
	const D2D1_RECT_F rect = RectangleFromPRectangle(rc);
	const D2D1_RECT_F rectFill = RectangleInset(rect, fillStroke.stroke.WidthF());
	const float halfStroke = fillStroke.stroke.WidthF() / 2.0f;
	const D2D1_RECT_F rectOutline = RectangleInset(rect, halfStroke);

	D2DPenColourAlpha(fillStroke.fill.colour);
	pRenderTarget->FillRectangle(&rectFill, pBrush);
	D2DPenColourAlpha(fillStroke.stroke.colour);
	pRenderTarget->DrawRectangle(&rectOutline, pBrush, fillStroke.stroke.WidthF());
}

void SurfaceD2D::RectangleFrame(PRectangle rc, Stroke stroke) {
	if (pRenderTarget) {
		const XYPOSITION halfStroke = stroke.width / 2.0f;
		const D2D1_RECT_F rectangle1 = RectangleFromPRectangle(rc.Inset(halfStroke));
		D2DPenColourAlpha(stroke.colour);
		pRenderTarget->DrawRectangle(&rectangle1, pBrush, stroke.WidthF());
	}
}

void SurfaceD2D::FillRectangle(PRectangle rc, Fill fill) {
	if (pRenderTarget) {
		D2DPenColourAlpha(fill.colour);
		const D2D1_RECT_F rectangle = RectangleFromPRectangleEx(rc);
		pRenderTarget->FillRectangle(&rectangle, pBrush);
	}
}

void SurfaceD2D::FillRectangleAligned(PRectangle rc, Fill fill) {
	FillRectangle(PixelAlign(rc, 1), fill);
}

void SurfaceD2D::FillRectangle(PRectangle rc, Surface &surfacePattern) {
	SurfaceD2D *psurfOther = down_cast<SurfaceD2D *>(&surfacePattern);
	PLATFORM_ASSERT(psurfOther);
	ID2D1Bitmap *pBitmap = nullptr;
	HRESULT hr = psurfOther->GetBitmap(&pBitmap);
	if (SUCCEEDED(hr) && pBitmap) {
		ID2D1BitmapBrush *pBitmapBrush = nullptr;
		const D2D1_BITMAP_BRUSH_PROPERTIES brushProperties =
			D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP,
				D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
		// Create the bitmap brush.
		hr = pRenderTarget->CreateBitmapBrush(pBitmap, brushProperties, &pBitmapBrush);
		ReleaseUnknown(pBitmap);
		if (SUCCEEDED(hr) && pBitmapBrush) {
			pRenderTarget->FillRectangle(RectangleFromPRectangleEx(rc), pBitmapBrush);
			ReleaseUnknown(pBitmapBrush);
		}
	}
}

void SurfaceD2D::RoundedRectangle(PRectangle rc, FillStroke fillStroke) {
	if (pRenderTarget) {
		D2D1_ROUNDED_RECT roundedRectFill = {
			RectangleFromPRectangle(rc.Inset(1.0)),
			4, 4 };
		D2DPenColourAlpha(fillStroke.fill.colour);
		pRenderTarget->FillRoundedRectangle(roundedRectFill, pBrush);

		D2D1_ROUNDED_RECT roundedRect = {
			RectangleFromPRectangle(rc.Inset(0.5)),
			4, 4 };
		D2DPenColourAlpha(fillStroke.stroke.colour);
		pRenderTarget->DrawRoundedRectangle(roundedRect, pBrush, fillStroke.stroke.WidthF());
	}
}

void SurfaceD2D::AlphaRectangle(PRectangle rc, XYPOSITION cornerSize, FillStroke fillStroke) {
	const D2D1_RECT_F rect = RectangleFromPRectangle(rc);
	const D2D1_RECT_F rectFill = RectangleInset(rect, fillStroke.stroke.WidthF());
	const float halfStroke = fillStroke.stroke.WidthF() / 2.0f;
	const D2D1_RECT_F rectOutline = RectangleInset(rect, halfStroke);
	if (pRenderTarget) {
		if (cornerSize == 0) {
			// When corner size is zero, draw square rectangle to prevent blurry pixels at corners
			D2DPenColourAlpha(fillStroke.fill.colour);
			pRenderTarget->FillRectangle(rectFill, pBrush);

			D2DPenColourAlpha(fillStroke.stroke.colour);
			pRenderTarget->DrawRectangle(rectOutline, pBrush, fillStroke.stroke.WidthF());
		} else {
			const float cornerSizeF = static_cast<float>(cornerSize);
			D2D1_ROUNDED_RECT roundedRectFill = {
				rectFill, cornerSizeF - 1.0f, cornerSizeF - 1.0f };
			D2DPenColourAlpha(fillStroke.fill.colour);
			pRenderTarget->FillRoundedRectangle(roundedRectFill, pBrush);

			D2D1_ROUNDED_RECT roundedRect = { rectOutline, cornerSizeF, cornerSizeF };
			D2DPenColourAlpha(fillStroke.stroke.colour);
			pRenderTarget->DrawRoundedRectangle(roundedRect, pBrush, fillStroke.stroke.WidthF());
		}
	}
}

void SurfaceD2D::GradientRectangle(PRectangle rc, const std::vector<ColourStop> &stops, GradientOptions options) {
	if (pRenderTarget) {
		D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lgbp {
			DPointFromPoint(Point(rc.left, rc.top)), {}
		};
		switch (options) {
		case GradientOptions::leftToRight:
			lgbp.endPoint = DPointFromPoint(Point(rc.right, rc.top));
			break;
		case GradientOptions::topToBottom:
		default:
			lgbp.endPoint = DPointFromPoint(Point(rc.left, rc.bottom));
			break;
		}

		std::vector<D2D1_GRADIENT_STOP> gradientStops;
		for (const ColourStop &stop : stops) {
			gradientStops.push_back({ static_cast<FLOAT>(stop.position), ColorFromColourAlpha(stop.colour) });
		}
		ID2D1GradientStopCollection *pGradientStops = nullptr;
		HRESULT hr = pRenderTarget->CreateGradientStopCollection(
			gradientStops.data(), static_cast<UINT32>(gradientStops.size()), &pGradientStops);
		if (FAILED(hr) || !pGradientStops) {
			return;
		}
		ID2D1LinearGradientBrush *pBrushLinear = nullptr;
		hr = pRenderTarget->CreateLinearGradientBrush(
			lgbp, pGradientStops, &pBrushLinear);
		if (SUCCEEDED(hr) && pBrushLinear) {
			const D2D1_RECT_F rectangle = RectangleFromPRectangle(PRectangle(
				std::round(rc.left), rc.top, std::round(rc.right), rc.bottom));
			pRenderTarget->FillRectangle(&rectangle, pBrushLinear);
			ReleaseUnknown(pBrushLinear);
		}
		ReleaseUnknown(pGradientStops);
	}
}

void SurfaceD2D::DrawRGBAImage(PRectangle rc, int width, int height, const unsigned char *pixelsImage) {
	if (pRenderTarget) {
		if (rc.Width() > width)
			rc.left += std::floor((rc.Width() - width) / 2);
		rc.right = rc.left + width;
		if (rc.Height() > height)
			rc.top += std::floor((rc.Height() - height) / 2);
		rc.bottom = rc.top + height;

		std::vector<unsigned char> image(RGBAImage::bytesPerPixel * height * width);
		RGBAImage::BGRAFromRGBA(image.data(), pixelsImage, static_cast<ptrdiff_t>(height) * width);

		ID2D1Bitmap *bitmap = nullptr;
		const D2D1_SIZE_U size = D2D1::SizeU(width, height);
		D2D1_BITMAP_PROPERTIES props = { {DXGI_FORMAT_B8G8R8A8_UNORM,
			D2D1_ALPHA_MODE_PREMULTIPLIED}, 72.0, 72.0 };
		const HRESULT hr = pRenderTarget->CreateBitmap(size, image.data(),
			width * 4, &props, &bitmap);
		if (SUCCEEDED(hr)) {
			const D2D1_RECT_F rcDestination = RectangleFromPRectangleEx(rc);
			pRenderTarget->DrawBitmap(bitmap, rcDestination);
			ReleaseUnknown(bitmap);
		}
	}
}

void SurfaceD2D::Ellipse(PRectangle rc, FillStroke fillStroke) {
	if (!pRenderTarget)
		return;
	const D2D1_POINT_2F centre = DPointFromPoint(rc.Centre());

	const FLOAT radiusFill = static_cast<FLOAT>(rc.Width() / 2.0f - fillStroke.stroke.width);
	const D2D1_ELLIPSE ellipseFill = { centre, radiusFill, radiusFill };

	D2DPenColourAlpha(fillStroke.fill.colour);
	pRenderTarget->FillEllipse(ellipseFill, pBrush);

	const FLOAT radiusOutline = static_cast<FLOAT>(rc.Width() / 2.0f - fillStroke.stroke.width / 2.0f);
	const D2D1_ELLIPSE ellipseOutline = { centre, radiusOutline, radiusOutline };

	D2DPenColourAlpha(fillStroke.stroke.colour);
	pRenderTarget->DrawEllipse(ellipseOutline, pBrush, fillStroke.stroke.WidthF());
}

void SurfaceD2D::Stadium(PRectangle rc, FillStroke fillStroke, Ends ends) {
	if (!pRenderTarget)
		return;
	if (rc.Width() < rc.Height()) {
		// Can't draw nice ends so just draw a rectangle
		RectangleDraw(rc, fillStroke);
		return;
	}
	const FLOAT radius = static_cast<FLOAT>(rc.Height() / 2.0);
	const FLOAT radiusFill = radius - fillStroke.stroke.WidthF();
	const FLOAT halfStroke = fillStroke.stroke.WidthF() / 2.0f;
	if (ends == Surface::Ends::semiCircles) {
		const D2D1_RECT_F rect = RectangleFromPRectangle(rc);
		D2D1_ROUNDED_RECT roundedRectFill = { RectangleInset(rect, fillStroke.stroke.WidthF()),
			radiusFill, radiusFill };
		D2DPenColourAlpha(fillStroke.fill.colour);
		pRenderTarget->FillRoundedRectangle(roundedRectFill, pBrush);

		D2D1_ROUNDED_RECT roundedRect = { RectangleInset(rect, halfStroke),
			radius, radius };
		D2DPenColourAlpha(fillStroke.stroke.colour);
		pRenderTarget->DrawRoundedRectangle(roundedRect, pBrush, fillStroke.stroke.WidthF());
	} else {
		const Ends leftSide = static_cast<Ends>(static_cast<int>(ends) & 0xf);
		const Ends rightSide = static_cast<Ends>(static_cast<int>(ends) & 0xf0);
		PRectangle rcInner = rc;
		rcInner.left += radius;
		rcInner.right -= radius;
		ID2D1PathGeometry *pathGeometry = nullptr;
		const HRESULT hrGeometry = pD2DFactory->CreatePathGeometry(&pathGeometry);
		if (FAILED(hrGeometry) || !pathGeometry)
			return;
		ID2D1GeometrySink *pSink = nullptr;
		const HRESULT hrSink = pathGeometry->Open(&pSink);
		if (SUCCEEDED(hrSink) && pSink) {
			switch (leftSide) {
			case Ends::leftFlat:
				pSink->BeginFigure(DPointFromPoint(Point(rc.left + halfStroke, rc.top + halfStroke)), D2D1_FIGURE_BEGIN_FILLED);
				pSink->AddLine(DPointFromPoint(Point(rc.left + halfStroke, rc.bottom - halfStroke)));
				break;
			case Ends::leftAngle:
				pSink->BeginFigure(DPointFromPoint(Point(rcInner.left + halfStroke, rc.top + halfStroke)), D2D1_FIGURE_BEGIN_FILLED);
				pSink->AddLine(DPointFromPoint(Point(rc.left + halfStroke, rc.Centre().y)));
				pSink->AddLine(DPointFromPoint(Point(rcInner.left + halfStroke, rc.bottom - halfStroke)));
				break;
			case Ends::semiCircles:
			default: {
				pSink->BeginFigure(DPointFromPoint(Point(rcInner.left + halfStroke, rc.top + halfStroke)), D2D1_FIGURE_BEGIN_FILLED);
				D2D1_ARC_SEGMENT segment{};
				segment.point = DPointFromPoint(Point(rcInner.left + halfStroke, rc.bottom - halfStroke));
				segment.size = D2D1::SizeF(radiusFill, radiusFill);
				segment.rotationAngle = 0.0f;
				segment.sweepDirection = D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;
				segment.arcSize = D2D1_ARC_SIZE_SMALL;
				pSink->AddArc(segment);
			}
			break;
			}

			switch (rightSide) {
			case Ends::rightFlat:
				pSink->AddLine(DPointFromPoint(Point(rc.right - halfStroke, rc.bottom - halfStroke)));
				pSink->AddLine(DPointFromPoint(Point(rc.right - halfStroke, rc.top + halfStroke)));
				break;
			case Ends::rightAngle:
				pSink->AddLine(DPointFromPoint(Point(rcInner.right - halfStroke, rc.bottom - halfStroke)));
				pSink->AddLine(DPointFromPoint(Point(rc.right - halfStroke, rc.Centre().y)));
				pSink->AddLine(DPointFromPoint(Point(rcInner.right - halfStroke, rc.top + halfStroke)));
				break;
			case Ends::semiCircles:
			default: {
				pSink->AddLine(DPointFromPoint(Point(rcInner.right - halfStroke, rc.bottom - halfStroke)));
				D2D1_ARC_SEGMENT segment{};
				segment.point = DPointFromPoint(Point(rcInner.right - halfStroke, rc.top + halfStroke));
				segment.size = D2D1::SizeF(radiusFill, radiusFill);
				segment.rotationAngle = 0.0f;
				segment.sweepDirection = D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;
				segment.arcSize = D2D1_ARC_SIZE_SMALL;
				pSink->AddArc(segment);
			}
			break;
			}

			pSink->EndFigure(D2D1_FIGURE_END_CLOSED);
			pSink->Close();
		}

		ReleaseUnknown(pSink);
		D2DPenColourAlpha(fillStroke.fill.colour);
		pRenderTarget->FillGeometry(pathGeometry, pBrush);
		D2DPenColourAlpha(fillStroke.stroke.colour);
		pRenderTarget->DrawGeometry(pathGeometry, pBrush, fillStroke.stroke.WidthF());
		ReleaseUnknown(pathGeometry);
 	}
 }

void SurfaceD2D::Copy(PRectangle rc, Point from, Surface &surfaceSource) {
	SurfaceD2D &surfOther = down_cast<SurfaceD2D &>(surfaceSource);
	ID2D1Bitmap *pBitmap = nullptr;
	const HRESULT hr = surfOther.GetBitmap(&pBitmap);
	if (SUCCEEDED(hr) && pBitmap) {
		const D2D1_RECT_F rcDestination = RectangleFromPRectangle(rc);
		const D2D1_RECT_F rcSource = RectangleFromPRectangle(PRectangle(
			from.x, from.y, from.x + rc.Width(), from.y + rc.Height()));
		pRenderTarget->DrawBitmap(pBitmap, rcDestination, 1.0f,
			D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, rcSource);
		ReleaseUnknown(pBitmap);
	}
}

class BlobInline final : public IDWriteInlineObject {
	XYPOSITION width;

	// IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, PVOID *ppv) noexcept override;
	STDMETHODIMP_(ULONG)AddRef() noexcept override;
	STDMETHODIMP_(ULONG)Release() noexcept override;

	// IDWriteInlineObject
	COM_DECLSPEC_NOTHROW STDMETHODIMP Draw(
		void *clientDrawingContext,
		IDWriteTextRenderer *renderer,
		FLOAT originX,
		FLOAT originY,
		BOOL isSideways,
		BOOL isRightToLeft,
		IUnknown *clientDrawingEffect
	) override;
	COM_DECLSPEC_NOTHROW STDMETHODIMP GetMetrics(DWRITE_INLINE_OBJECT_METRICS *metrics) override;
	COM_DECLSPEC_NOTHROW STDMETHODIMP GetOverhangMetrics(DWRITE_OVERHANG_METRICS *overhangs) override;
	COM_DECLSPEC_NOTHROW STDMETHODIMP GetBreakConditions(
		DWRITE_BREAK_CONDITION *breakConditionBefore,
		DWRITE_BREAK_CONDITION *breakConditionAfter) override;
public:
	explicit BlobInline(XYPOSITION width_ = 0.0f) noexcept : width(width_) {}
	virtual ~BlobInline() noexcept = default;
};

/// Implement IUnknown
STDMETHODIMP BlobInline::QueryInterface(REFIID riid, PVOID *ppv) noexcept {
	if (!ppv) {
		return E_POINTER;
	}
	// Never called so not checked.
	*ppv = nullptr;
	if (riid == IID_IUnknown)
		*ppv = this;
	if (riid == __uuidof(IDWriteInlineObject))
		*ppv = this;
	if (!*ppv)
		return E_NOINTERFACE;
	return S_OK;
}

STDMETHODIMP_(ULONG) BlobInline::AddRef() noexcept {
	// Lifetime tied to Platform methods so ignore any reference operations.
	return 1;
}

STDMETHODIMP_(ULONG) BlobInline::Release() noexcept {
	// Lifetime tied to Platform methods so ignore any reference operations.
	return 1;
}

/// Implement IDWriteInlineObject
COM_DECLSPEC_NOTHROW HRESULT STDMETHODCALLTYPE BlobInline::Draw(
	void*,
	IDWriteTextRenderer*,
	FLOAT,
	FLOAT,
	BOOL,
	BOOL,
	IUnknown*) {
	// Since not performing drawing, not necessary to implement
	// Could be implemented by calling back into platform-independent code.
	// This would allow more of the drawing to be mediated through DirectWrite.
	return S_OK;
}

COM_DECLSPEC_NOTHROW HRESULT STDMETHODCALLTYPE BlobInline::GetMetrics(
	DWRITE_INLINE_OBJECT_METRICS *metrics
) {
	if (!metrics) {
		return E_POINTER;
	}
	metrics->width = static_cast<FLOAT>(width);
	metrics->height = 2;
	metrics->baseline = 1;
	metrics->supportsSideways = FALSE;
	return S_OK;
}

COM_DECLSPEC_NOTHROW HRESULT STDMETHODCALLTYPE BlobInline::GetOverhangMetrics(
	DWRITE_OVERHANG_METRICS *overhangs
) {
	if (!overhangs) {
		return E_POINTER;
	}
	overhangs->left = 0;
	overhangs->top = 0;
	overhangs->right = 0;
	overhangs->bottom = 0;
	return S_OK;
}

COM_DECLSPEC_NOTHROW HRESULT STDMETHODCALLTYPE BlobInline::GetBreakConditions(
	DWRITE_BREAK_CONDITION *breakConditionBefore,
	DWRITE_BREAK_CONDITION *breakConditionAfter
) {
	if (!breakConditionBefore || !breakConditionAfter) {
		return E_POINTER;
	}
	// Since not performing 2D layout, not necessary to implement
	*breakConditionBefore = DWRITE_BREAK_CONDITION_NEUTRAL;
	*breakConditionAfter = DWRITE_BREAK_CONDITION_NEUTRAL;
	return S_OK;
}

class ScreenLineLayout final : public IScreenLineLayout {
	IDWriteTextLayout *textLayout = nullptr;
	std::string text;
	std::wstring buffer;
	std::vector<BlobInline> blobs;
	static void FillTextLayoutFormats(const IScreenLine *screenLine, IDWriteTextLayout *textLayout, std::vector<BlobInline> &blobs);
	static std::wstring ReplaceRepresentation(std::string_view text);
	static size_t GetPositionInLayout(std::string_view text, size_t position) noexcept;
public:
	explicit ScreenLineLayout(const IScreenLine *screenLine);
	// Deleted so ScreenLineLayout objects can not be copied
	ScreenLineLayout(const ScreenLineLayout &) = delete;
	ScreenLineLayout(ScreenLineLayout &&) = delete;
	ScreenLineLayout &operator=(const ScreenLineLayout &) = delete;
	ScreenLineLayout &operator=(ScreenLineLayout &&) = delete;
	~ScreenLineLayout() noexcept override;
	size_t PositionFromX(XYPOSITION xDistance, bool charPosition) noexcept override;
	XYPOSITION XFromPosition(size_t caretPosition) noexcept override;
	std::vector<Interval> FindRangeIntervals(size_t start, size_t end) override;
};

// Each char can have its own style, so we fill the textLayout with the textFormat of each char

void ScreenLineLayout::FillTextLayoutFormats(const IScreenLine *screenLine, IDWriteTextLayout *textLayout, std::vector<BlobInline> &blobs) {
	// Reserve enough entries up front so they are not moved and the pointers handed
	// to textLayout remain valid.
	const ptrdiff_t numRepresentations = screenLine->RepresentationCount();
	std::string_view text = screenLine->Text();
	const ptrdiff_t numTabs = std::count(std::begin(text), std::end(text), '\t');
	blobs.reserve(numRepresentations + numTabs);

	UINT32 layoutPosition = 0;

	for (size_t bytePosition = 0; bytePosition < screenLine->Length();) {
		const unsigned char uch = screenLine->Text()[bytePosition];
		const unsigned int byteCount = UTF8BytesOfLead(uch);
		const UINT32 codeUnits = UTF16LengthFromUTF8ByteCount(byteCount);
		const DWRITE_TEXT_RANGE textRange = { layoutPosition, codeUnits };

		XYPOSITION representationWidth = screenLine->RepresentationWidth(bytePosition);
		if ((representationWidth == 0.0f) && (screenLine->Text()[bytePosition] == '\t')) {
			D2D1_POINT_2F realPt {};
			DWRITE_HIT_TEST_METRICS realCaretMetrics {};
			textLayout->HitTestTextPosition(
				layoutPosition,
				false, // trailing if false, else leading edge
				&realPt.x,
				&realPt.y,
				&realCaretMetrics
			);

			const XYPOSITION nextTab = screenLine->TabPositionAfter(realPt.x);
			representationWidth = nextTab - realPt.x;
		}
		if (representationWidth > 0.0f) {
			blobs.emplace_back(representationWidth);
			textLayout->SetInlineObject(&blobs.back(), textRange);
		};

		const FontDirectWrite *pfm = down_cast<const FontDirectWrite *>(screenLine->FontOfPosition(bytePosition));

		const UINT32 fontFamilyNameSize = pfm->pTextFormat->GetFontFamilyNameLength();
		std::wstring fontFamilyName(fontFamilyNameSize + 1, L'\0');

		const HRESULT hrFamily = pfm->pTextFormat->GetFontFamilyName(fontFamilyName.data(), fontFamilyNameSize + 1);
		if (SUCCEEDED(hrFamily)) {
			textLayout->SetFontFamilyName(fontFamilyName.c_str(), textRange);
		}

		textLayout->SetFontSize(pfm->pTextFormat->GetFontSize(), textRange);
		textLayout->SetFontWeight(pfm->pTextFormat->GetFontWeight(), textRange);
		textLayout->SetFontStyle(pfm->pTextFormat->GetFontStyle(), textRange);

		const UINT32 localeNameSize = pfm->pTextFormat->GetLocaleNameLength();
		std::wstring localeName(localeNameSize + 1, L'\0');

		const HRESULT hrLocale = pfm->pTextFormat->GetLocaleName(localeName.data(), localeNameSize);
		if (SUCCEEDED(hrLocale)) {
			textLayout->SetLocaleName(localeName.c_str(), textRange);
		}

		textLayout->SetFontStretch(pfm->pTextFormat->GetFontStretch(), textRange);

		IDWriteFontCollection *fontCollection = nullptr;
		if (SUCCEEDED(pfm->pTextFormat->GetFontCollection(&fontCollection))) {
			textLayout->SetFontCollection(fontCollection, textRange);
		}

		bytePosition += byteCount;
		layoutPosition += codeUnits;
	}

}

/* Convert to a wide character string and replace tabs with X to stop DirectWrite tab expansion */

std::wstring ScreenLineLayout::ReplaceRepresentation(std::string_view text) {
	const TextWide wideText(text, SC_CP_UTF8);
	std::wstring ws(wideText.buffer, wideText.tlen);
	std::replace(ws.begin(), ws.end(), L'\t', L'X');
	return ws;
}

// Finds the position in the wide character version of the text.

size_t ScreenLineLayout::GetPositionInLayout(std::string_view text, size_t position) noexcept {
	const std::string_view textUptoPosition = text.substr(0, position);
	return UTF16Length(textUptoPosition);
}

ScreenLineLayout::ScreenLineLayout(const IScreenLine *screenLine) {
	// If the text is empty, then no need to go through this function
	if (!screenLine || !screenLine->Length()) {
		return;
	}

	text = screenLine->Text();

	// Get textFormat
	const FontDirectWrite *pfm = down_cast<const FontDirectWrite *>(screenLine->FontOfPosition(0));

	if (!pIDWriteFactory || !pfm->pTextFormat) {
		return;
	}

	// Convert the string to wstring and replace the original control characters with their representative chars.
	buffer = ReplaceRepresentation(screenLine->Text());

	// Create a text layout
	const HRESULT hrCreate = pIDWriteFactory->CreateTextLayout(buffer.c_str(),
		static_cast<UINT32>(buffer.length()),
		pfm->pTextFormat,
		static_cast<FLOAT>(screenLine->Width()),
		static_cast<FLOAT>(screenLine->Height()),
		&textLayout);
	if (!SUCCEEDED(hrCreate)) {
		return;
	}

	// Fill the textLayout chars with their own formats
	FillTextLayoutFormats(screenLine, textLayout, blobs);
}

ScreenLineLayout::~ScreenLineLayout() noexcept {
	ReleaseUnknown(textLayout);
}

// Get the position from the provided x

size_t ScreenLineLayout::PositionFromX(XYPOSITION xDistance, bool charPosition) noexcept {
	if (!textLayout) {
		return 0;
	}

	// Returns the text position corresponding to the mouse (x, y).
	// If hitting the trailing side of a cluster, return the
	// leading edge of the following text position.

	BOOL isTrailingHit = FALSE;
	BOOL isInside = FALSE;
	DWRITE_HIT_TEST_METRICS caretMetrics {};

	textLayout->HitTestPoint(
		static_cast<FLOAT>(xDistance),
		0.0f,
		&isTrailingHit,
		&isInside,
		&caretMetrics
	);

	DWRITE_HIT_TEST_METRICS hitTestMetrics {};
	if (isTrailingHit) {
		FLOAT caretX = 0.0f;
		FLOAT caretY = 0.0f;

		// Uses hit-testing to align the current caret position to a whole cluster,
		// rather than residing in the middle of a base character + diacritic,
		// surrogate pair, or character + UVS.

		// Align the caret to the nearest whole cluster.
		textLayout->HitTestTextPosition(
			caretMetrics.textPosition,
			false,
			&caretX,
			&caretY,
			&hitTestMetrics
		);
	}

	size_t pos;
	if (charPosition) {
		pos = isTrailingHit ? hitTestMetrics.textPosition : caretMetrics.textPosition;
	} else {
		pos = isTrailingHit ? hitTestMetrics.textPosition + hitTestMetrics.length : caretMetrics.textPosition;
	}

	// Get the character position in original string
	return UTF8PositionFromUTF16Position(text, pos);
}

// Finds the point of the caret position

XYPOSITION ScreenLineLayout::XFromPosition(size_t caretPosition) noexcept {
	if (!textLayout) {
		return 0.0;
	}
	// Convert byte positions to wchar_t positions
	const size_t position = GetPositionInLayout(text, caretPosition);

	// Translate text character offset to point (x, y).
	DWRITE_HIT_TEST_METRICS caretMetrics {};
	D2D1_POINT_2F pt {};

	textLayout->HitTestTextPosition(
		static_cast<UINT32>(position),
		false, // trailing if false, else leading edge
		&pt.x,
		&pt.y,
		&caretMetrics
	);

	return pt.x;
}

// Find the selection range rectangles

std::vector<Interval> ScreenLineLayout::FindRangeIntervals(size_t start, size_t end) {
	std::vector<Interval> ret;

	if (!textLayout || (start == end)) {
		return ret;
	}

	// Convert byte positions to wchar_t positions
	const size_t startPos = GetPositionInLayout(text, start);
	const size_t endPos = GetPositionInLayout(text, end);

	// Find selection range length
	const size_t rangeLength = (endPos > startPos) ? (endPos - startPos) : (startPos - endPos);

	// Determine actual number of hit-test ranges
	UINT32 actualHitTestCount = 0;

	// First try with 2 elements and if more needed, allocate.
	std::vector<DWRITE_HIT_TEST_METRICS> hitTestMetrics(2);
	textLayout->HitTestTextRange(
		static_cast<UINT32>(startPos),
		static_cast<UINT32>(rangeLength),
		0, // x
		0, // y
		hitTestMetrics.data(),
		static_cast<UINT32>(hitTestMetrics.size()),
		&actualHitTestCount
	);

	if (actualHitTestCount == 0) {
		return ret;
	}

	if (hitTestMetrics.size() < actualHitTestCount) {
		// Allocate enough room to return all hit-test metrics.
		hitTestMetrics.resize(actualHitTestCount);
		textLayout->HitTestTextRange(
			static_cast<UINT32>(startPos),
			static_cast<UINT32>(rangeLength),
			0, // x
			0, // y
			hitTestMetrics.data(),
			static_cast<UINT32>(hitTestMetrics.size()),
			&actualHitTestCount
		);
	}

	// Get the selection ranges behind the text.
	for (size_t i = 0; i < actualHitTestCount; ++i) {
		// Store selection rectangle
		const DWRITE_HIT_TEST_METRICS &htm = hitTestMetrics[i];
		const Interval selectionInterval { htm.left, htm.left + htm.width };
		ret.push_back(selectionInterval);
	}

	return ret;
}

std::unique_ptr<IScreenLineLayout> SurfaceD2D::Layout(const IScreenLine *screenLine) {
	return std::make_unique<ScreenLineLayout>(screenLine);
}

void SurfaceD2D::DrawTextCommon(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text, int codePageOverride, UINT fuOptions) {
	SetFont(font_);

	// Use Unicode calls
	const int codePageDraw = codePageOverride ? codePageOverride : mode.codePage;
	const TextWide tbuf(text, codePageDraw);
	if (pRenderTarget && pTextFormat && pBrush) {
		if (fuOptions & ETO_CLIPPED) {
			const D2D1_RECT_F rcClip = RectangleFromPRectangle(rc);
			pRenderTarget->PushAxisAlignedClip(rcClip, D2D1_ANTIALIAS_MODE_ALIASED);
		}

		// Explicitly creating a text layout appears a little faster
		IDWriteTextLayout *pTextLayout = nullptr;
		const HRESULT hr = pIDWriteFactory->CreateTextLayout(tbuf.buffer, tbuf.tlen,
			pTextFormat,
			static_cast<FLOAT>(rc.Width()),
			static_cast<FLOAT>(rc.Height()),
			&pTextLayout);
		if (SUCCEEDED(hr)) {
			D2D1_POINT_2F origin = DPointFromPoint(Point(rc.left, ybase - yAscent));
			pRenderTarget->DrawTextLayout(origin, pTextLayout, pBrush, d2dDrawTextOptions);
			ReleaseUnknown(pTextLayout);
		}

		if (fuOptions & ETO_CLIPPED) {
			pRenderTarget->PopAxisAlignedClip();
		}
	}
}

void SurfaceD2D::DrawTextNoClip(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text,
	ColourAlpha fore, ColourAlpha back) {
	if (pRenderTarget) {
		FillRectangleAligned(rc, back);
		D2DPenColourAlpha(fore);
		DrawTextCommon(rc, font_, ybase, text, 0, ETO_OPAQUE);
	}
}

void SurfaceD2D::DrawTextClipped(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text,
	ColourAlpha fore, ColourAlpha back) {
	if (pRenderTarget) {
		FillRectangleAligned(rc, back);
		D2DPenColourAlpha(fore);
		DrawTextCommon(rc, font_, ybase, text, 0, ETO_OPAQUE | ETO_CLIPPED);
	}
}

void SurfaceD2D::DrawTextTransparent(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text,
	ColourAlpha fore) {
	// Avoid drawing spaces in transparent mode
	for (const char ch : text) {
		if (ch != ' ') {
			if (pRenderTarget) {
				D2DPenColourAlpha(fore);
				DrawTextCommon(rc, font_, ybase, text, 0, 0);
			}
			return;
		}
	}
}

void SurfaceD2D::MeasureWidths(const Font *font_, std::string_view text, XYPOSITION *positions) {
	SetFont(font_);
	if (!pIDWriteFactory || !pTextFormat) {
		// SetFont failed or no access to DirectWrite so give up.
		return;
	}
	const TextWide tbuf(text, mode.codePage);
	TextPositions poses(tbuf.tlen);
	// Initialize poses for safety.
	std::fill(poses.buffer, poses.buffer + tbuf.tlen, 0.0f);
	// Create a layout
	IDWriteTextLayout *pTextLayout = nullptr;
	const HRESULT hrCreate = pIDWriteFactory->CreateTextLayout(tbuf.buffer, tbuf.tlen, pTextFormat, 10000.0, 1000.0, &pTextLayout);
	if (!SUCCEEDED(hrCreate) || !pTextLayout) {
		return;
	}
	constexpr int clusters = stackBufferLength;
	DWRITE_CLUSTER_METRICS clusterMetrics[clusters];
	UINT32 count = 0;
	const HRESULT hrGetCluster = pTextLayout->GetClusterMetrics(clusterMetrics, clusters, &count);
	ReleaseUnknown(pTextLayout);
	if (!SUCCEEDED(hrGetCluster)) {
		return;
	}
	// A cluster may be more than one WCHAR, such as for "ffi" which is a ligature in the Candara font
	XYPOSITION position = 0.0;
	int ti = 0;
	for (unsigned int ci = 0; ci < count; ci++) {
		const FLOAT width = clusterMetrics[ci].width;
		const UINT16 length = clusterMetrics[ci].length;
		for (UINT16 inCluster = 0; inCluster < length; inCluster++) {
			poses.buffer[ti++] = position + width * (inCluster + 1) / length;
		}
		position += width;
	}
	PLATFORM_ASSERT(ti == tbuf.tlen);
	if (mode.codePage == SC_CP_UTF8) {
		// Map the widths given for UTF-16 characters back onto the UTF-8 input string
		size_t i = 0;
		for (int ui = 0; ui < tbuf.tlen; ui++) {
			const unsigned char uch = text[i];
			const unsigned int byteCount = UTF8BytesOfLead(uch);
			if (byteCount == 4) {	// Non-BMP
				ui++;
			}
			for (unsigned int bytePos = 0; (bytePos < byteCount) && (i < text.length()) && (ui < tbuf.tlen); bytePos++) {
				positions[i++] = poses.buffer[ui];
			}
		}
		const XYPOSITION lastPos = (i > 0) ? positions[i - 1] : 0.0;
		while (i < text.length()) {
			positions[i++] = lastPos;
		}
	} else {
		const DBCSCharClassify *dbcs = DBCSCharClassify::Get(mode.codePage);
		if (dbcs) {
			// May be one or two bytes per position
			int ui = 0;
			for (size_t i = 0; i < text.length() && ui < tbuf.tlen;) {
				positions[i] = poses.buffer[ui];
				if (dbcs->IsLeadByte(text[i])) {
					positions[i + 1] = poses.buffer[ui];
					i += 2;
				} else {
					i++;
				}

				ui++;
			}
		} else {
			// One char per position
			PLATFORM_ASSERT(text.length() == static_cast<size_t>(tbuf.tlen));
			for (int kk = 0; kk < tbuf.tlen; kk++) {
				positions[kk] = poses.buffer[kk];
			}
		}
	}
}

XYPOSITION SurfaceD2D::WidthText(const Font *font_, std::string_view text) {
	FLOAT width = 1.0;
	SetFont(font_);
	const TextWide tbuf(text, mode.codePage);
	if (pIDWriteFactory && pTextFormat) {
		// Create a layout
		IDWriteTextLayout *pTextLayout = nullptr;
		const HRESULT hr = pIDWriteFactory->CreateTextLayout(tbuf.buffer, tbuf.tlen, pTextFormat, 1000.0, 1000.0, &pTextLayout);
		if (SUCCEEDED(hr) && pTextLayout) {
			DWRITE_TEXT_METRICS textMetrics;
			if (SUCCEEDED(pTextLayout->GetMetrics(&textMetrics)))
				width = textMetrics.widthIncludingTrailingWhitespace;
			ReleaseUnknown(pTextLayout);
		}
	}
	return width;
}

void SurfaceD2D::DrawTextNoClipUTF8(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text,
	ColourAlpha fore, ColourAlpha back) {
	if (pRenderTarget) {
		FillRectangleAligned(rc, back);
		D2DPenColourAlpha(fore);
		DrawTextCommon(rc, font_, ybase, text, SC_CP_UTF8, ETO_OPAQUE);
	}
}

void SurfaceD2D::DrawTextClippedUTF8(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text,
	ColourAlpha fore, ColourAlpha back) {
	if (pRenderTarget) {
		FillRectangleAligned(rc, back);
		D2DPenColourAlpha(fore);
		DrawTextCommon(rc, font_, ybase, text, SC_CP_UTF8, ETO_OPAQUE | ETO_CLIPPED);
	}
}

void SurfaceD2D::DrawTextTransparentUTF8(PRectangle rc, const Font *font_, XYPOSITION ybase, std::string_view text,
	ColourAlpha fore) {
	// Avoid drawing spaces in transparent mode
	for (const char ch : text) {
		if (ch != ' ') {
			if (pRenderTarget) {
				D2DPenColourAlpha(fore);
				DrawTextCommon(rc, font_, ybase, text, SC_CP_UTF8, 0);
			}
			return;
		}
	}
}

void SurfaceD2D::MeasureWidthsUTF8(const Font *font_, std::string_view text, XYPOSITION *positions) {
	SetFont(font_);
	if (!pIDWriteFactory || !pTextFormat) {
		// SetFont failed or no access to DirectWrite so give up.
		return;
	}
	const TextWide tbuf(text, SC_CP_UTF8);
	TextPositions poses(tbuf.tlen);
	// Initialize poses for safety.
	std::fill(poses.buffer, poses.buffer + tbuf.tlen, 0.0f);
	// Create a layout
	IDWriteTextLayout *pTextLayout = nullptr;
	const HRESULT hrCreate = pIDWriteFactory->CreateTextLayout(tbuf.buffer, tbuf.tlen, pTextFormat, 10000.0, 1000.0, &pTextLayout);
	if (!SUCCEEDED(hrCreate) || !pTextLayout) {
		return;
	}
	constexpr int clusters = stackBufferLength;
	DWRITE_CLUSTER_METRICS clusterMetrics[clusters];
	UINT32 count = 0;
	const HRESULT hrGetCluster = pTextLayout->GetClusterMetrics(clusterMetrics, clusters, &count);
	ReleaseUnknown(pTextLayout);
	if (!SUCCEEDED(hrGetCluster)) {
		return;
	}
	// A cluster may be more than one WCHAR, such as for "ffi" which is a ligature in the Candara font
	XYPOSITION position = 0.0f;
	int ti = 0;
	for (unsigned int ci = 0; ci < count; ci++) {
		const FLOAT width = clusterMetrics[ci].width;
		const UINT16 length = clusterMetrics[ci].length;
		for (UINT16 inCluster = 0; inCluster < length; inCluster++) {
			poses.buffer[ti++] = position + width * (inCluster + 1) / length;
		}
		position += width;
	}
	PLATFORM_ASSERT(ti == tbuf.tlen);
	// Map the widths given for UTF-16 characters back onto the UTF-8 input string
	size_t i = 0;
	for (int ui = 0; ui < tbuf.tlen; ui++) {
		const unsigned char uch = text[i];
		const unsigned int byteCount = UTF8BytesOfLead(uch);
		if (byteCount == 4) {	// Non-BMP
			ui++;
			PLATFORM_ASSERT(ui < ti);
		}
		for (unsigned int bytePos = 0; (bytePos < byteCount) && (i < text.length()); bytePos++) {
			positions[i++] = poses.buffer[ui];
		}

	}
	const XYPOSITION lastPos = (i > 0) ? positions[i - 1] : 0.0;
	while (i < text.length()) {
		positions[i++] = lastPos;
	}
}

XYPOSITION SurfaceD2D::WidthTextUTF8(const Font * font_, std::string_view text) {
	FLOAT width = 1.0;
	SetFont(font_);
	const TextWide tbuf(text, SC_CP_UTF8);
	if (pIDWriteFactory && pTextFormat) {
		// Create a layout
		IDWriteTextLayout *pTextLayout = nullptr;
		const HRESULT hr = pIDWriteFactory->CreateTextLayout(tbuf.buffer, tbuf.tlen, pTextFormat, 1000.0, 1000.0, &pTextLayout);
		if (SUCCEEDED(hr)) {
			DWRITE_TEXT_METRICS textMetrics;
			if (SUCCEEDED(pTextLayout->GetMetrics(&textMetrics)))
				width = textMetrics.widthIncludingTrailingWhitespace;
			ReleaseUnknown(pTextLayout);
		}
	}
	return width;
}

XYPOSITION SurfaceD2D::Ascent(const Font *font_) noexcept {
	SetFont(font_);
	return std::ceil(yAscent);
}

XYPOSITION SurfaceD2D::Descent(const Font *font_) noexcept {
	SetFont(font_);
	return std::ceil(yDescent);
}

XYPOSITION SurfaceD2D::InternalLeading(const Font *font_) noexcept {
	SetFont(font_);
	return std::floor(yInternalLeading);
}

XYPOSITION SurfaceD2D::Height(const Font *font_) noexcept {
	return Ascent(font_) + Descent(font_);
}

XYPOSITION SurfaceD2D::AverageCharWidth(const Font *font_) {
	FLOAT width = 1.0;
	SetFont(font_);
	if (pIDWriteFactory && pTextFormat) {
		// Create a layout
		IDWriteTextLayout *pTextLayout = nullptr;
		constexpr const WCHAR *wszAllAlpha = L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
		constexpr int lenAllAlpha = 52;
		//static_assert(lenAllAlpha == __builtin_wcslen(wszAllAlpha));
		const HRESULT hr = pIDWriteFactory->CreateTextLayout(wszAllAlpha, lenAllAlpha,
			pTextFormat, 1000.0, 1000.0, &pTextLayout);
		if (SUCCEEDED(hr) && pTextLayout) {
			DWRITE_TEXT_METRICS textMetrics;
			if (SUCCEEDED(pTextLayout->GetMetrics(&textMetrics)))
				width = textMetrics.width / lenAllAlpha;
			ReleaseUnknown(pTextLayout);
		}
	}
	return width;
}

void SurfaceD2D::SetClip(PRectangle rc) noexcept {
	if (pRenderTarget) {
		const D2D1_RECT_F rcClip = RectangleFromPRectangle(rc);
		pRenderTarget->PushAxisAlignedClip(rcClip, D2D1_ANTIALIAS_MODE_ALIASED);
		clipsActive++;
	}
}

void SurfaceD2D::PopClip() noexcept {
	if (pRenderTarget) {
		PLATFORM_ASSERT(clipsActive > 0);
		pRenderTarget->PopAxisAlignedClip();
		clipsActive--;
	}
}

void SurfaceD2D::FlushCachedState() noexcept {
}

void SurfaceD2D::FlushDrawing() noexcept {
	if (pRenderTarget) {
		pRenderTarget->Flush();
	}
}

#endif

std::unique_ptr<Surface> Surface::Allocate(int technology) {
#if defined(USE_D2D)
	if (technology == SCWIN_TECH_GDI)
		return std::make_unique<SurfaceGDI>();
	else
		return std::make_unique<SurfaceD2D>();
#else
	return std::make_unique<SurfaceGDI>();
#endif
}

void Window::Destroy() noexcept {
	if (wid)
		::DestroyWindow(HwndFromWindowID(wid));
	wid = nullptr;
}

PRectangle Window::GetPosition() const noexcept {
	RECT rc;
	::GetWindowRect(HwndFromWindowID(wid), &rc);
	return PRectangleFromRectEx(rc);
}

void Window::SetPosition(PRectangle rc) noexcept {
	::SetWindowPos(HwndFromWindowID(wid),
		nullptr, static_cast<int>(rc.left), static_cast<int>(rc.top),
		static_cast<int>(rc.Width()), static_cast<int>(rc.Height()), SWP_NOZORDER | SWP_NOACTIVATE);
}

namespace {

RECT RectFromMonitor(HMONITOR hMonitor) noexcept {
	MONITORINFO mi = {};
	mi.cbSize = sizeof(mi);
	if (GetMonitorInfo(hMonitor, &mi)) {
		return mi.rcWork;
	}
	RECT rc {};
	if (::SystemParametersInfo(SPI_GETWORKAREA, 0, &rc, 0) == 0) {
		rc.left = 0;
		rc.top = 0;
		rc.right = 0;
		rc.bottom = 0;
	}
	return rc;
}

}

void Window::SetPositionRelative(PRectangle rc, const Window *relativeTo) noexcept {
	const DWORD style = GetWindowStyle(HwndFromWindowID(wid));
	if (style & WS_POPUP) {
		POINT ptOther {};
		::ClientToScreen(HwndFromWindow(*relativeTo), &ptOther);
		rc.Move(static_cast<XYPOSITION>(ptOther.x), static_cast<XYPOSITION>(ptOther.y));

		const RECT rcMonitor = RectFromPRectangleEx(rc);

		HMONITOR hMonitor = MonitorFromRect(&rcMonitor, MONITOR_DEFAULTTONEAREST);
		// If hMonitor is NULL, that's just the main screen anyways.
		const RECT rcWork = RectFromMonitor(hMonitor);

		if (rcWork.left < rcWork.right) {
			// Now clamp our desired rectangle to fit inside the work area
			// This way, the menu will fit wholly on one screen. An improvement even
			// if you don't have a second monitor on the left... Menu's appears half on
			// one screen and half on the other are just U.G.L.Y.!
			if (rc.right > rcWork.right)
				rc.Move(rcWork.right - rc.right, 0);
			if (rc.bottom > rcWork.bottom)
				rc.Move(0, rcWork.bottom - rc.bottom);
			if (rc.left < rcWork.left)
				rc.Move(rcWork.left - rc.left, 0);
			if (rc.top < rcWork.top)
				rc.Move(0, rcWork.top - rc.top);
		}
	}
	SetPosition(rc);
}

PRectangle Window::GetClientPosition() const noexcept {
	RECT rc {};
	if (wid)
		::GetClientRect(HwndFromWindowID(wid), &rc);
	return PRectangleFromRectEx(rc);
}

void Window::Show(bool show) const noexcept {
	if (show)
		::ShowWindow(HwndFromWindowID(wid), SW_SHOWNOACTIVATE);
	else
		::ShowWindow(HwndFromWindowID(wid), SW_HIDE);
}

void Window::InvalidateAll() noexcept {
	::InvalidateRect(HwndFromWindowID(wid), nullptr, FALSE);
}

void Window::InvalidateRectangle(PRectangle rc) noexcept {
	const RECT rcw = RectFromPRectangleEx(rc);
	::InvalidateRect(HwndFromWindowID(wid), &rcw, FALSE);
}

namespace {

void FlipBitmap(HBITMAP bitmap, int width, int height) noexcept {
	HDC hdc = ::CreateCompatibleDC({});
	if (hdc) {
		HBITMAP prevBmp = SelectBitmap(hdc, bitmap);
		::StretchBlt(hdc, width - 1, 0, -width, height, hdc, 0, 0, width, height, SRCCOPY);
		SelectBitmap(hdc, prevBmp);
		::DeleteDC(hdc);
	}
}

}

HCURSOR LoadReverseArrowCursor(UINT dpi) noexcept {
	HCURSOR reverseArrowCursor {};

	bool created = false;
	HCURSOR cursor = ::LoadCursor({}, IDC_ARROW);

	if (dpi != g_uSystemDPI) {
		const int width = SystemMetricsForDpi(SM_CXCURSOR, dpi);
		const int height = SystemMetricsForDpi(SM_CYCURSOR, dpi);
		HCURSOR copy = static_cast<HCURSOR>(::CopyImage(cursor, IMAGE_CURSOR, width, height, LR_COPYFROMRESOURCE | LR_COPYRETURNORG));
		if (copy) {
			created = copy != cursor;
			cursor = copy;
		}
	}

	ICONINFO info;
	if (::GetIconInfo(cursor, &info)) {
		BITMAP bmp{};
		if (::GetObject(info.hbmMask, sizeof(bmp), &bmp)) {
			FlipBitmap(info.hbmMask, bmp.bmWidth, bmp.bmHeight);
			if (info.hbmColor)
				FlipBitmap(info.hbmColor, bmp.bmWidth, bmp.bmHeight);
			info.xHotspot = bmp.bmWidth - 1 - info.xHotspot;

			reverseArrowCursor = ::CreateIconIndirect(&info);
		}

		::DeleteObject(info.hbmMask);
		if (info.hbmColor)
			::DeleteObject(info.hbmColor);
	}

	if (created) {
		::DestroyCursor(cursor);
	}
	return reverseArrowCursor;
}

void Window::SetCursor(Cursor curs) noexcept {
	switch (curs) {
	case Cursor::text:
		::SetCursor(::LoadCursor({}, IDC_IBEAM));
		break;
	case Cursor::up:
		::SetCursor(::LoadCursor({}, IDC_UPARROW));
		break;
	case Cursor::wait:
		::SetCursor(::LoadCursor({}, IDC_WAIT));
		break;
	case Cursor::horizontal:
		::SetCursor(::LoadCursor({}, IDC_SIZEWE));
		break;
	case Cursor::vertical:
		::SetCursor(::LoadCursor({}, IDC_SIZENS));
		break;
	case Cursor::hand:
		::SetCursor(::LoadCursor({}, IDC_HAND));
		break;
	case Cursor::reverseArrow:
	case Cursor::arrow:
	case Cursor::invalid:	// Should not occur, but just in case.
		::SetCursor(::LoadCursor({}, IDC_ARROW));
		break;
	}
}

/* Returns rectangle of monitor pt is on, both rect and pt are in Window's
   coordinates */
PRectangle Window::GetMonitorRect(Point pt) const noexcept {
	const PRectangle rcPosition = GetPosition();
	POINT ptDesktop = { static_cast<LONG>(pt.x + rcPosition.left),
		static_cast<LONG>(pt.y + rcPosition.top) };
	HMONITOR hMonitor = MonitorFromPoint(ptDesktop, MONITOR_DEFAULTTONEAREST);

	const RECT rcWork = RectFromMonitor(hMonitor);
	if (rcWork.left < rcWork.right) {
		const PRectangle rcMonitor(
			rcWork.left - rcPosition.left,
			rcWork.top - rcPosition.top,
			rcWork.right - rcPosition.left,
			rcWork.bottom - rcPosition.top);
		return rcMonitor;
	} else {
		return PRectangle();
	}
}

struct ListItemData {
	const char *text;
	int pixId;
};

class LineToItem {
	std::vector<char> words;

	std::vector<ListItemData> data;

public:
	void Clear() noexcept {
		words.clear();
		data.clear();
	}

	ListItemData Get(size_t index) const noexcept {
		if (index < data.size()) {
			return data[index];
		} else {
			ListItemData missing = { "", -1 };
			return missing;
		}
	}
	int Count() const noexcept {
		return static_cast<int>(data.size());
	}

	void AllocItem(const char *text, int pixId) {
		ListItemData lid = { text, pixId };
		data.push_back(lid);
	}

	char *SetWords(const char *s, size_t length) {
		words = std::vector<char>(s, s + length + 1);
		return words.data();
	}
};

static constexpr const WCHAR *ListBoxX_ClassName = L"ListBoxX";
#define LISTBOXX_USE_THICKFRAME		0
#define LISTBOXX_USE_BORDER			1
#define LISTBOXX_USE_FAKE_FRAME		0

class ListBoxX final : public ListBox {
	int lineHeight = 10;
	HFONT fontCopy{};
	int technology = SCWIN_TECH_GDI;
	RGBAImageSet images;
	LineToItem lti;
	HWND lb{};
	bool unicodeMode = false;
	int desiredVisibleRows = 9;
	unsigned int maxItemCharacters = 0;
	unsigned int aveCharWidth = 8;
	COLORREF colorText = 0;
	COLORREF colorBackground = 0;
	COLORREF colorHighlight = 0;
	COLORREF colorHighlightText = 0;
	Window *parent = nullptr;
	int ctrlID = 0;
	UINT dpi = USER_DEFAULT_SCREEN_DPI;
	IListBoxDelegate *delegate = nullptr;
	const char *widestItem = nullptr;
	unsigned int maxCharWidth = 1;
	WPARAM resizeHit = 0;
	PRectangle rcPreSize;
	Point dragOffset;
	Point location;	// Caret location at which the list is opened
	int wheelDelta = 0; // mouse wheel residue

	HWND GetHWND() const noexcept;
	void AppendListItem(const char *text, const char *numword);
	static void AdjustWindowRect(PRectangle *rc, UINT dpi) noexcept;
	int ItemHeight() const;
	int MinClientWidth() const noexcept;
	int TextOffset() const;
	POINT GetClientExtent() const noexcept;
	POINT MinTrackSize() const;
	POINT MaxTrackSize() const;
	void SetRedraw(bool on) noexcept;
	void OnDoubleClick();
	void OnSelChange();
	void ResizeToCursor();
	void StartResize(WPARAM) noexcept;
	LRESULT NcHitTest(WPARAM, LPARAM) const noexcept;
	void CentreItem(int n);
	void Paint(HDC) noexcept;
	static LRESULT CALLBACK ControlWndProc(HWND hWnd, UINT iMessage, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

	static constexpr Point ItemInset {0, 0};	// Padding around whole item
	static constexpr Point TextInset {2, 0};	// Padding around text
	static constexpr Point ImageInset {1, 0};	// Padding around image

public:
	ListBoxX() noexcept = default;
	ListBoxX(const ListBoxX &) = delete;
	ListBoxX(ListBoxX &&) = delete;
	ListBoxX &operator=(const ListBoxX &) = delete;
	ListBoxX &operator=(ListBoxX &&) = delete;
	~ListBoxX() noexcept override {
		if (fontCopy) {
			::DeleteObject(fontCopy);
			fontCopy = nullptr;
		}
	}
	void SetFont(const Font *font) noexcept override;
	void SCICALL Create(Window &parent_, int ctrlID_, Point location_, int lineHeight_, bool unicodeMode_, int technology_) noexcept override;
	void SetAverageCharWidth(int width) noexcept override;
	void SetVisibleRows(int rows) noexcept override;
	int GetVisibleRows() const noexcept override;
	PRectangle GetDesiredRect() override;
	int CaretFromEdge() const override;
	void Clear() noexcept override;
	void Append(const char *s, int type = -1) const noexcept override;
	int Length() const noexcept override;
	void Select(int n) override;
	int GetSelection() const noexcept override;
	int Find(const char *prefix) const noexcept override;
	std::string GetValue(int n) const override;
	void RegisterImage(int type, const char *xpm_data) override;
	void RegisterRGBAImage(int type, int width, int height, const unsigned char *pixelsImage) override;
	void ClearRegisteredImages() noexcept override;
	void SetDelegate(IListBoxDelegate *lbDelegate) noexcept override;
	void SetList(const char *list, char separator, char typesep) override;
	void SCICALL SetOptions(ListOptions options_) noexcept override;
	void Draw(const DRAWITEMSTRUCT *pDrawItem);
	LRESULT WndProc(HWND hWnd, UINT iMessage, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK StaticWndProc(HWND hWnd, UINT iMessage, WPARAM wParam, LPARAM lParam);
};

#if LISTBOXX_USE_FAKE_FRAME
constexpr int ListBoxXFakeFrameSize = 4;
#endif

std::unique_ptr<ListBox> ListBox::Allocate() {
	return std::make_unique<ListBoxX>();
}

void ListBoxX::Create(Window &parent_, int ctrlID_, Point location_, int lineHeight_, bool unicodeMode_, int technology_) noexcept {
	parent = &parent_;
	ctrlID = ctrlID_;
	location = location_;
	lineHeight = lineHeight_;
	unicodeMode = unicodeMode_;
	technology = technology_;
	HWND hwndParent = HwndFromWindow(*parent);
	HINSTANCE hinstanceParent = GetWindowInstance(hwndParent);
	// Window created as popup so not clipped within parent client area
	wid = ::CreateWindowEx(
		WS_EX_WINDOWEDGE, ListBoxX_ClassName, L"",
#if LISTBOXX_USE_THICKFRAME
		WS_POPUP | WS_THICKFRAME,
#elif LISTBOXX_USE_BORDER
		WS_POPUP | WS_BORDER,
#else
		WS_POPUP,
#endif
		100, 100, 150, 80, hwndParent,
		{},
		hinstanceParent,
		this);

	dpi = GetWindowDPI(hwndParent);
	POINT locationw = POINTFromPointEx(location);
	::MapWindowPoints(hwndParent, {}, &locationw, 1);
	location = PointFromPOINTEx(locationw);
}

void ListBoxX::SetFont(const Font *font) noexcept {
	const FontWin *pfm = down_cast<const FontWin *>(font);
	if (pfm) {
		if (fontCopy) {
			::DeleteObject(fontCopy);
		}
		fontCopy = pfm->HFont();
		SetWindowFont(lb, fontCopy, FALSE);
	}
}

void ListBoxX::SetAverageCharWidth(int width) noexcept {
	aveCharWidth = width;
}

void ListBoxX::SetVisibleRows(int rows) noexcept {
	desiredVisibleRows = rows;
}

int ListBoxX::GetVisibleRows() const noexcept {
	return desiredVisibleRows;
}

HWND ListBoxX::GetHWND() const noexcept {
	return HwndFromWindowID(GetID());
}

PRectangle ListBoxX::GetDesiredRect() {
	PRectangle rcDesired = GetPosition();

	int rows = Length();
	if ((rows == 0) || (rows > desiredVisibleRows))
		rows = desiredVisibleRows;
	rcDesired.bottom = rcDesired.top + ItemHeight() * rows;

	int width = MinClientWidth();
	HDC hdc = ::GetDC(lb);
	HFONT oldFont = SelectFont(hdc, fontCopy);
	SIZE textSize {};
	int len = 0;
	if (widestItem) {
		len = static_cast<int>(strlen(widestItem));
		if (unicodeMode) {
			const TextWide tbuf(widestItem, SC_CP_UTF8);
			::GetTextExtentPoint32W(hdc, tbuf.buffer, tbuf.tlen, &textSize);
		} else {
			::GetTextExtentPoint32A(hdc, widestItem, len, &textSize);
		}
	}

	TEXTMETRIC tm;
	::GetTextMetrics(hdc, &tm);
	maxCharWidth = tm.tmMaxCharWidth;
	SelectFont(hdc, oldFont);
	::ReleaseDC(lb, hdc);

	const int widthDesired = std::max(textSize.cx, (len + 1) * tm.tmAveCharWidth);
	if (width < widthDesired)
		width = widthDesired;

	rcDesired.right = rcDesired.left + TextOffset() + width + (TextInset.x * 2);
	if (Length() > rows) {
		rcDesired.right += SystemMetricsForDpi(SM_CXVSCROLL, dpi);
	}

	AdjustWindowRect(&rcDesired, dpi);
	return rcDesired;
}

int ListBoxX::TextOffset() const {
	const int pixWidth = images.GetWidth();
	return static_cast<int>(pixWidth == 0 ? ItemInset.x : ItemInset.x + pixWidth + (ImageInset.x * 2));
}

int ListBoxX::CaretFromEdge() const {
	PRectangle rc;
	AdjustWindowRect(&rc, dpi);
	return TextOffset() + static_cast<int>(TextInset.x + (0 - rc.left) - 1);
}

void ListBoxX::Clear() noexcept {
	ListBox_ResetContent(lb);
	maxItemCharacters = 0;
	widestItem = nullptr;
	lti.Clear();
}

void ListBoxX::Append(const char *, int) const noexcept {
	// This method is no longer called in Scintilla
	PLATFORM_ASSERT(false);
}

int ListBoxX::Length() const noexcept {
	return lti.Count();
}

void ListBoxX::Select(int n) {
	// We are going to scroll to centre on the new selection and then select it, so disable
	// redraw to avoid flicker caused by a painting new selection twice in unselected and then
	// selected states
	SetRedraw(false);
	CentreItem(n);
	ListBox_SetCurSel(lb, n);
	OnSelChange();
	SetRedraw(true);
}

int ListBoxX::GetSelection() const noexcept {
	return ListBox_GetCurSel(lb);
}

// This is not actually called at present
int ListBoxX::Find(const char *) const noexcept {
	return LB_ERR;
}

std::string ListBoxX::GetValue(int n) const {
	const ListItemData item = lti.Get(n);
	return item.text;
}

void ListBoxX::RegisterImage(int type, const char *xpm_data) {
	XPM xpmImage(xpm_data);
	images.AddImage(type, std::make_unique<RGBAImage>(xpmImage));
}

void ListBoxX::RegisterRGBAImage(int type, int width, int height, const unsigned char *pixelsImage) {
	images.AddImage(type, std::make_unique<RGBAImage>(width, height, 1.0f, pixelsImage));
}

void ListBoxX::ClearRegisteredImages() noexcept {
	images.Clear();
}

namespace {

inline COLORREF ColourOfElement(std::optional<ColourAlpha> colour, UINT nIndex) noexcept {
	if (colour.has_value()) {
		return colour.value().OpaqueRGB();
	} else {
		return ::GetSysColor(nIndex);
	}
}

void FillRectColour(HDC hdc, const RECT *lprc, COLORREF colour) noexcept {
	HBRUSH brush = ::CreateSolidBrush(colour);
	::FillRect(hdc, lprc, brush);
	::DeleteObject(brush);
}

}

void ListBoxX::Draw(const DRAWITEMSTRUCT *pDrawItem) {
	if ((pDrawItem->itemAction == ODA_SELECT) || (pDrawItem->itemAction == ODA_DRAWENTIRE)) {
		RECT rcBox = pDrawItem->rcItem;
		rcBox.left += TextOffset();
		if (pDrawItem->itemState & ODS_SELECTED) {
			RECT rcImage = pDrawItem->rcItem;
			rcImage.right = rcBox.left;
			// The image is not highlighted
			FillRectColour(pDrawItem->hDC, &rcImage, colorBackground);
			FillRectColour(pDrawItem->hDC, &rcBox, colorHighlight);
			::SetBkColor(pDrawItem->hDC, colorHighlight);
			::SetTextColor(pDrawItem->hDC, colorHighlightText);
		} else {
			FillRectColour(pDrawItem->hDC, &pDrawItem->rcItem, colorBackground);
			::SetBkColor(pDrawItem->hDC, colorBackground);
			::SetTextColor(pDrawItem->hDC, colorText);
		}

		const ListItemData item = lti.Get(pDrawItem->itemID);
		const int pixId = item.pixId;
		const char *text = item.text;
		const int len = static_cast<int>(strlen(text));

		RECT rcText = rcBox;
		::InsetRect(&rcText, static_cast<int>(TextInset.x), static_cast<int>(TextInset.y));

		if (unicodeMode) {
			const TextWide tbuf(text, SC_CP_UTF8);
			::DrawTextW(pDrawItem->hDC, tbuf.buffer, tbuf.tlen, &rcText, DT_NOPREFIX | DT_END_ELLIPSIS | DT_SINGLELINE | DT_NOCLIP);
		} else {
			::DrawTextA(pDrawItem->hDC, text, len, &rcText, DT_NOPREFIX | DT_END_ELLIPSIS | DT_SINGLELINE | DT_NOCLIP);
		}

		// Draw the image, if any
		const RGBAImage *pimage = images.Get(pixId);
		if (pimage) {
			std::unique_ptr<Surface> surfaceItem(Surface::Allocate(technology));
			if (technology == SCWIN_TECH_GDI) {
				surfaceItem->Init(pDrawItem->hDC, pDrawItem->hwndItem);
				const long left = pDrawItem->rcItem.left + static_cast<int>(ItemInset.x + ImageInset.x);
				const PRectangle rcImage = PRectangle::FromInts(left, pDrawItem->rcItem.top,
					left + images.GetWidth(), pDrawItem->rcItem.bottom);
				surfaceItem->DrawRGBAImage(rcImage,
					pimage->GetWidth(), pimage->GetHeight(), pimage->Pixels());
				::SetTextAlign(pDrawItem->hDC, TA_TOP);
			} else {
#if defined(USE_D2D)
				const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
					D2D1_RENDER_TARGET_TYPE_DEFAULT,
					D2D1::PixelFormat(
						DXGI_FORMAT_B8G8R8A8_UNORM,
						D2D1_ALPHA_MODE_IGNORE),
					0,
					0,
					D2D1_RENDER_TARGET_USAGE_NONE,
					D2D1_FEATURE_LEVEL_DEFAULT
				);
				ID2D1DCRenderTarget *pDCRT = nullptr;
				HRESULT hr = pD2DFactory->CreateDCRenderTarget(&props, &pDCRT);
				if (SUCCEEDED(hr) && pDCRT) {
					RECT rcWindow;
					GetClientRect(pDrawItem->hwndItem, &rcWindow);
					hr = pDCRT->BindDC(pDrawItem->hDC, &rcWindow);
					if (SUCCEEDED(hr)) {
						surfaceItem->Init(pDCRT, pDrawItem->hwndItem);
						pDCRT->BeginDraw();
						const long left = pDrawItem->rcItem.left + static_cast<long>(ItemInset.x + ImageInset.x);
						const PRectangle rcImage = PRectangle::FromInts(left, pDrawItem->rcItem.top,
							left + images.GetWidth(), pDrawItem->rcItem.bottom);
						surfaceItem->DrawRGBAImage(rcImage,
							pimage->GetWidth(), pimage->GetHeight(), pimage->Pixels());
						pDCRT->EndDraw();
						ReleaseUnknown(pDCRT);
					}
				}
#endif
			}
		}
	}
}

void ListBoxX::AppendListItem(const char *text, const char *numword) {
	int pixId = -1;
	if (numword) {
		pixId = 0;
		char ch;
		while ((ch = *++numword) != '\0') {
			pixId = 10 * pixId + (ch - '0');
		}
	}

	lti.AllocItem(text, pixId);
	const unsigned int len = static_cast<unsigned int>(strlen(text));
	if (maxItemCharacters < len) {
		maxItemCharacters = len;
		widestItem = text;
	}
}

void ListBoxX::SetDelegate(IListBoxDelegate *lbDelegate) noexcept {
	delegate = lbDelegate;
}

void ListBoxX::SetList(const char *list, const char separator, const char typesep) {
	// Turn off redraw while populating the list - this has a significant effect, even if
	// the listbox is not visible.
	SetRedraw(false);
	Clear();
	const size_t size = strlen(list);
	char *words = lti.SetWords(list, size);
	char *startword = words;
	char *numword = nullptr;
	for (size_t i = 0; i < size; i++) {
		if (words[i] == separator) {
			words[i] = '\0';
			if (numword)
				*numword = '\0';
			AppendListItem(startword, numword);
			startword = words + i + 1;
			numword = nullptr;
		} else if (words[i] == typesep) {
			numword = words + i;
		}
	}
	if (startword) {
		if (numword)
			*numword = '\0';
		AppendListItem(startword, numword);
	}

	// Finally populate the listbox itself with the correct number of items
	const int count = lti.Count();
	::SendMessage(lb, LB_INITSTORAGE, count, 0);
	for (intptr_t j = 0; j < count; j++) {
		ListBox_AddItemData(lb, j + 1);
	}
	SetRedraw(true);
}

void ListBoxX::SetOptions(ListOptions options_) noexcept {
	colorText = ColourOfElement(options_.fore, COLOR_WINDOWTEXT);
	colorBackground = ColourOfElement(options_.back, COLOR_WINDOW);
	colorHighlight = ColourOfElement(options_.backSelected, COLOR_HIGHLIGHT);
	colorHighlightText = ColourOfElement(options_.foreSelected, COLOR_HIGHLIGHTTEXT);
}

void ListBoxX::AdjustWindowRect(PRectangle *rc, UINT dpi) noexcept {
	RECT rcw = RectFromPRectangleEx(*rc);
#if LISTBOXX_USE_THICKFRAME
	AdjustWindowRectForDpi(&rcw, WS_THICKFRAME, WS_EX_WINDOWEDGE, dpi);
#elif LISTBOXX_USE_BORDER
	AdjustWindowRectForDpi(&rcw, WS_BORDER, WS_EX_WINDOWEDGE, dpi);
#else
	AdjustWindowRectForDpi(&rcw, 0, WS_EX_WINDOWEDGE, dpi);
#endif
	*rc = PRectangleFromRectEx(rcw);
#if LISTBOXX_USE_FAKE_FRAME
	*rc = rc->Inflate(ListBoxXFakeFrameSize, ListBoxXFakeFrameSize);
#endif
}

int ListBoxX::ItemHeight() const {
	int itemHeight = lineHeight + (static_cast<int>(TextInset.y) * 2);
	const int pixHeight = images.GetHeight() + (static_cast<int>(ImageInset.y) * 2);
	if (itemHeight < pixHeight) {
		itemHeight = pixHeight;
	}
	return itemHeight;
}

int ListBoxX::MinClientWidth() const noexcept {
	return 12 * (aveCharWidth + aveCharWidth / 3);
}

POINT ListBoxX::MinTrackSize() const {
	PRectangle rc = PRectangle::FromInts(0, 0, MinClientWidth(), ItemHeight());
	AdjustWindowRect(&rc, dpi);
	POINT ret = { static_cast<LONG>(rc.Width()), static_cast<LONG>(rc.Height()) };
	return ret;
}

POINT ListBoxX::MaxTrackSize() const {
	const int width = maxCharWidth * maxItemCharacters + static_cast<int>(TextInset.x) * 2 +
		TextOffset() + SystemMetricsForDpi(SM_CXVSCROLL, dpi);
	PRectangle rc = PRectangle::FromInts(0, 0,
		std::max(MinClientWidth(), width),
		ItemHeight() * lti.Count());
	AdjustWindowRect(&rc, dpi);
	POINT ret = { static_cast<LONG>(rc.Width()), static_cast<LONG>(rc.Height()) };
	return ret;
}

void ListBoxX::SetRedraw(bool on) noexcept {
	::SendMessage(lb, WM_SETREDRAW, on, 0);
	if (on)
		::InvalidateRect(lb, nullptr, TRUE);
}

void ListBoxX::ResizeToCursor() {
	PRectangle rc = GetPosition();
	POINT ptw;
	::GetCursorPos(&ptw);
	const Point pt = PointFromPOINT(ptw) + dragOffset;

	switch (resizeHit) {
	case HTLEFT:
		rc.left = pt.x;
		break;
	case HTRIGHT:
		rc.right = pt.x;
		break;
	case HTTOP:
		rc.top = pt.y;
		break;
	case HTTOPLEFT:
		rc.top = pt.y;
		rc.left = pt.x;
		break;
	case HTTOPRIGHT:
		rc.top = pt.y;
		rc.right = pt.x;
		break;
	case HTBOTTOM:
		rc.bottom = pt.y;
		break;
	case HTBOTTOMLEFT:
		rc.bottom = pt.y;
		rc.left = pt.x;
		break;
	case HTBOTTOMRIGHT:
		rc.bottom = pt.y;
		rc.right = pt.x;
		break;
	}

	const POINT ptMin = MinTrackSize();
	const POINT ptMax = MaxTrackSize();
	// We don't allow the left edge to move at present, but just in case
	rc.left = std::clamp(rc.left, rcPreSize.right - ptMax.x, rcPreSize.right - ptMin.x);
	rc.top = std::clamp(rc.top, rcPreSize.bottom - ptMax.y, rcPreSize.bottom - ptMin.y);
	rc.right = std::clamp(rc.right, rcPreSize.left + ptMin.x, rcPreSize.left + ptMax.x);
	rc.bottom = std::clamp(rc.bottom, rcPreSize.top + ptMin.y, rcPreSize.top + ptMax.y);

	SetPosition(rc);
}

void ListBoxX::StartResize(WPARAM hitCode) noexcept {
	rcPreSize = GetPosition();
	POINT cursorPos;
	::GetCursorPos(&cursorPos);

	switch (hitCode) {
	case HTRIGHT:
	case HTBOTTOM:
	case HTBOTTOMRIGHT:
		dragOffset.x = rcPreSize.right - cursorPos.x;
		dragOffset.y = rcPreSize.bottom - cursorPos.y;
		break;

	case HTTOPRIGHT:
		dragOffset.x = rcPreSize.right - cursorPos.x;
		dragOffset.y = rcPreSize.top - cursorPos.y;
		break;

		// Note that the current hit test code prevents the left edge cases ever firing
		// as we don't want the left edge to be movable
	case HTLEFT:
	case HTTOP:
	case HTTOPLEFT:
		dragOffset.x = rcPreSize.left - cursorPos.x;
		dragOffset.y = rcPreSize.top - cursorPos.y;
		break;
	case HTBOTTOMLEFT:
		dragOffset.x = rcPreSize.left - cursorPos.x;
		dragOffset.y = rcPreSize.bottom - cursorPos.y;
		break;

	default:
		return;
	}

	::SetCapture(GetHWND());
	resizeHit = hitCode;
}

LRESULT ListBoxX::NcHitTest(WPARAM wParam, LPARAM lParam) const noexcept {
	const PRectangle rc = GetPosition();

	LRESULT hit = ::DefWindowProc(GetHWND(), WM_NCHITTEST, wParam, lParam);
	// There is an apparent bug in the DefWindowProc hit test code whereby it will
	// return HTTOPXXX if the window in question is shorter than the default
	// window caption height + frame, even if one is hovering over the bottom edge of
	// the frame, so workaround that here
	if (hit >= HTTOP && hit <= HTTOPRIGHT) {
		const int minHeight = SystemMetricsForDpi(SM_CYMINTRACK, dpi);
		const int yPos = GET_Y_LPARAM(lParam);
		if ((rc.Height() < minHeight) && (yPos > ((rc.top + rc.bottom) / 2))) {
			hit += HTBOTTOM - HTTOP;
		}
	}
#if LISTBOXX_USE_BORDER || LISTBOXX_USE_FAKE_FRAME
	else if (hit < HTSIZEFIRST || hit > HTSIZELAST) {
		const int cx = SystemMetricsForDpi(SM_CXVSCROLL, dpi);
#if LISTBOXX_USE_BORDER
		const PRectangle rcInner = rc.Deflate(SystemMetricsForDpi(SM_CXBORDER, dpi), SystemMetricsForDpi(SM_CYBORDER, dpi));
#else
		const PRectangle rcInner = rc.Deflate(ListBoxXFakeFrameSize, ListBoxXFakeFrameSize);
#endif
		const int xPos = GET_X_LPARAM(lParam);
		const int yPos = GET_Y_LPARAM(lParam);
		/*
		13 | 12 | 14         4 | 3 | 5
		10 |    | 11    =>   1 | 0 | 2
		16 | 15 | 17         7 | 6 | 8
		*/
		const int x = (xPos <= rcInner.left) ? 1 : ((xPos >= rcInner.right - cx) ? 2 : 0);
		int y = (yPos <= rcInner.top) ? 3 : ((yPos >= rcInner.bottom) ? 6 : 0);
		if (y == 0 && x == 2) {
			if (location.y < rc.top) {
				y = (yPos >= rcInner.bottom - cx) ? 6 : 0;
			} else {
				y = (yPos <= rcInner.top + cx) ? 3 : 0;
			}
		}
		const int h = x + y;
		hit = h ? (9 + h) : HTERROR;
	}
#endif

	// Never permit resizing that moves the left edge. Allow movement of top or bottom edge
	// depending on whether the list is above or below the caret
	switch (hit) {
	case HTLEFT:
	case HTTOPLEFT:
	case HTBOTTOMLEFT:
		hit = HTERROR;
		break;

	case HTTOP:
	case HTTOPRIGHT: {
		// Valid only if caret below list
		if (location.y < rc.top)
			hit = HTERROR;
	}
	break;

	case HTBOTTOM:
	case HTBOTTOMRIGHT: {
		// Valid only if caret above list
		if (rc.bottom <= location.y)
			hit = HTERROR;
	}
	break;
	}

	return hit;
}

void ListBoxX::OnDoubleClick() {
	if (delegate) {
		ListBoxEvent event(ListBoxEvent::EventType::doubleClick);
		delegate->ListNotify(&event);
	}
}

void ListBoxX::OnSelChange() {
	if (delegate) {
		ListBoxEvent event(ListBoxEvent::EventType::selectionChange);
		delegate->ListNotify(&event);
	}
}

POINT ListBoxX::GetClientExtent() const noexcept {
	RECT rc;
	::GetWindowRect(HwndFromWindowID(wid), &rc);
	POINT ret{ rc.right - rc.left, rc.bottom - rc.top };
	return ret;
}

void ListBoxX::CentreItem(int n) {
	// If below mid point, scroll up to centre, but with more items below if uneven
	if (n >= 0) {
		const POINT extent = GetClientExtent();
		const int visible = extent.y / ItemHeight();
		if (visible < Length()) {
			const int top = ListBox_GetTopIndex(lb);
			const int half = (visible - 1) / 2;
			if (n > (top + half))
				ListBox_SetTopIndex(lb, n - half);
		}
	}
}

// Performs a double-buffered paint operation to avoid flicker
void ListBoxX::Paint(HDC hDC) noexcept {
	const POINT extent = GetClientExtent();
	HBITMAP hBitmap = ::CreateCompatibleBitmap(hDC, extent.x, extent.y);
	HDC bitmapDC = ::CreateCompatibleDC(hDC);
	HBITMAP hBitmapOld = SelectBitmap(bitmapDC, hBitmap);
	// The list background is mainly erased during painting, but can be a small
	// unpainted area when at the end of a non-integrally sized list with a
	// vertical scroll bar
	const RECT rc = { 0, 0, extent.x, extent.y };
	FillRectColour(bitmapDC, &rc, colorBackground);
	// Paint the entire client area and vertical scrollbar
	::SendMessage(lb, WM_PRINT, reinterpret_cast<WPARAM>(bitmapDC), PRF_CLIENT | PRF_NONCLIENT);
	::BitBlt(hDC, 0, 0, extent.x, extent.y, bitmapDC, 0, 0, SRCCOPY);
	// Select a stock brush to prevent warnings from BoundsChecker
	SelectBrush(bitmapDC, GetStockBrush(WHITE_BRUSH));
	SelectBitmap(bitmapDC, hBitmapOld);
	::DeleteDC(bitmapDC);
	::DeleteObject(hBitmap);
}

LRESULT CALLBACK ListBoxX::ControlWndProc(HWND hWnd, UINT iMessage, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR /*dwRefData*/) {
	try {
		ListBoxX *lbx = static_cast<ListBoxX *>(PointerFromWindow(::GetParent(hWnd)));
		switch (iMessage) {
		case WM_ERASEBKGND:
			return TRUE;

		case WM_PAINT: {
			PAINTSTRUCT ps;
			HDC hDC = ::BeginPaint(hWnd, &ps);
			if (lbx) {
				lbx->Paint(hDC);
			}
			::EndPaint(hWnd, &ps);
		}
		return 0;

		case WM_MOUSEACTIVATE:
			// This prevents the view activating when the scrollbar is clicked
			return MA_NOACTIVATE;

		case WM_LBUTTONDOWN: {
			// We must take control of selection to prevent the ListBox activating
			// the popup
			const LRESULT lResult = ::SendMessage(hWnd, LB_ITEMFROMPOINT, 0, lParam);
			const int item = LOWORD(lResult);
			if (HIWORD(lResult) == 0 && item >= 0) {
				ListBox_SetCurSel(hWnd, item);
				if (lbx) {
					lbx->OnSelChange();
				}
			}
		}
		return 0;

		case WM_LBUTTONUP:
			return 0;

		case WM_LBUTTONDBLCLK: {
			if (lbx) {
				lbx->OnDoubleClick();
			}
		}
		return 0;

		case WM_MBUTTONDOWN:
			// disable the scroll wheel button click action
			return 0;

		case WM_NCDESTROY:
			RemoveWindowSubclass(hWnd, ControlWndProc, uIdSubclass);
			break;
		}
	} catch (...) {
	}
	return ::DefSubclassProc(hWnd, iMessage, wParam, lParam);
}

LRESULT ListBoxX::WndProc(HWND hWnd, UINT iMessage, WPARAM wParam, LPARAM lParam) {
	switch (iMessage) {
	case WM_CREATE: {
		HINSTANCE hinstanceParent = GetWindowInstance(HwndFromWindow(*parent));
		// Note that LBS_NOINTEGRALHEIGHT is specified to fix cosmetic issue when resizing the list
		// but has useful side effect of speeding up list population significantly
		lb = ::CreateWindowEx(
			0, L"listbox", L"",
			WS_CHILD | WS_VSCROLL | WS_VISIBLE |
			LBS_OWNERDRAWFIXED | LBS_NODATA | LBS_NOINTEGRALHEIGHT,
			0, 0, 150, 80, hWnd,
			reinterpret_cast<HMENU>(static_cast<ptrdiff_t>(ctrlID)),
			hinstanceParent,
			nullptr);
		::SetWindowSubclass(lb, ControlWndProc, 0, 0);
	}
	break;

	case WM_SIZE:
		if (lb) {
			SetRedraw(false);
			::SetWindowPos(lb, nullptr, 0, 0, LOWORD(lParam), HIWORD(lParam), SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
			// Ensure the selection remains visible
			CentreItem(GetSelection());
			SetRedraw(true);
		}
		break;

	case WM_PAINT: {
		PAINTSTRUCT ps;
		::BeginPaint(hWnd, &ps);
		::EndPaint(hWnd, &ps);
	}
	break;

	case WM_COMMAND:
		// This is not actually needed now - the registered double click action is used
		// directly to action a choice from the list.
		::SendMessage(HwndFromWindow(*parent), iMessage, wParam, lParam);
		break;

	case WM_MEASUREITEM: {
		MEASUREITEMSTRUCT *pMeasureItem = reinterpret_cast<MEASUREITEMSTRUCT *>(lParam);
		pMeasureItem->itemHeight = ItemHeight();
	}
	break;

	case WM_DRAWITEM:
		Draw(reinterpret_cast<DRAWITEMSTRUCT *>(lParam));
		break;

	case WM_DESTROY:
		lb = nullptr;
		SetWindowPointer(hWnd, nullptr);
		return ::DefWindowProc(hWnd, iMessage, wParam, lParam);

	case WM_ERASEBKGND:
		// To reduce flicker we can elide background erasure since this window is
		// completely covered by its child.
		return TRUE;

	case WM_GETMINMAXINFO: {
		MINMAXINFO *minMax = reinterpret_cast<MINMAXINFO*>(lParam);
		minMax->ptMaxTrackSize = MaxTrackSize();
		minMax->ptMinTrackSize = MinTrackSize();
	}
	break;

	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;

#if LISTBOXX_USE_FAKE_FRAME
	case WM_NCPAINT: {
		HDC hDC = ::GetWindowDC(hWnd);
		RECT rect;
		::GetClientRect(hWnd, &rect);

		// outer frame
		rect.right += 2*ListBoxXFakeFrameSize;
		rect.bottom += 2*ListBoxXFakeFrameSize;
		const int width = rect.right - rect.left;
		const int height = rect.bottom - rect.top;

		// inner border
		RECT client = rect;
		::InflateRect(&client, -ListBoxXFakeFrameSize + 1, -ListBoxXFakeFrameSize + 1);

		HDC hMemDC = CreateCompatibleDC(hDC);
		const BITMAPINFO bpih = { {sizeof(BITMAPINFOHEADER), width, height, 1, 32, BI_RGB, 0, 0, 0, 0, 0},
			{{0, 0, 0, 0}} };
		HBITMAP hbmMem = CreateDIBSection(hMemDC, &bpih, DIB_RGB_COLORS, nullptr, nullptr, 0);

		if (hbmMem) {
			HBITMAP hbmOld = SelectBitmap(hMemDC, hbmMem);
			BLENDFUNCTION merge = { AC_SRC_OVER, 0, 0, AC_SRC_ALPHA };

			GdiAlphaBlend(hDC, rect.left, rect.top, width, height, hMemDC, 0, 0, width, height, merge);

			SelectBitmap(hMemDC, hbmOld);
			::DeleteObject(hbmMem);
		}
		::DeleteDC(hMemDC);

		//HPEN hPen = ::CreatePen(PS_SOLID, 1, ::GetSysColor(COLOR_WINDOWFRAME));
		HPEN hPen = ::CreatePen(PS_SOLID, 1, RGB(255, 0, 0));
		HPEN hPenOld = SelectPen(hDC, hPen);
		::Rectangle(hDC, client.left, client.top, client.right, client.bottom);
		::SelectObject(hDC, hPenOld);
		::DeleteObject(hPen);

		::ReleaseDC(hWnd, hDC);
		return 0;
	}

	case WM_NCCALCSIZE: {
		LPRECT rect = reinterpret_cast<LPRECT>(lParam);
		::InflateRect(rect, -ListBoxXFakeFrameSize, -ListBoxXFakeFrameSize);
		return 0;
	}
#endif

	case WM_NCHITTEST:
		return NcHitTest(wParam, lParam);

	case WM_NCLBUTTONDOWN:
		// We have to implement our own window resizing because the DefWindowProc
		// implementation insists on activating the resized window
		StartResize(wParam);
		return 0;

	case WM_MOUSEMOVE: {
		if (resizeHit == 0) {
			return ::DefWindowProc(hWnd, iMessage, wParam, lParam);
		} else {
			ResizeToCursor();
		}
	}
	break;

	case WM_LBUTTONUP:
	case WM_CANCELMODE:
		if (resizeHit != 0) {
			resizeHit = 0;
			::ReleaseCapture();
		}
		return ::DefWindowProc(hWnd, iMessage, wParam, lParam);

	case WM_MOUSEWHEEL:
		wheelDelta -= GET_WHEEL_DELTA_WPARAM(wParam);
		if (std::abs(wheelDelta) >= WHEEL_DELTA) {
			const int nRows = GetVisibleRows();
			int linesToScroll = std::clamp(nRows - 1, 1, 3);
			linesToScroll *= (wheelDelta / WHEEL_DELTA);
			int top = ListBox_GetTopIndex(lb) + linesToScroll;
			if (top < 0) {
				top = 0;
			}
			ListBox_SetTopIndex(lb, top);
			// update wheel delta residue
			if (wheelDelta >= 0)
				wheelDelta = wheelDelta % WHEEL_DELTA;
			else
				wheelDelta = -(-wheelDelta % WHEEL_DELTA);
		}
		break;

	default:
		return ::DefWindowProc(hWnd, iMessage, wParam, lParam);
	}

	return 0;
}

LRESULT CALLBACK ListBoxX::StaticWndProc(
	HWND hWnd, UINT iMessage, WPARAM wParam, LPARAM lParam) {
	if (iMessage == WM_CREATE) {
		CREATESTRUCT *pCreate = reinterpret_cast<CREATESTRUCT *>(lParam);
		SetWindowPointer(hWnd, pCreate->lpCreateParams);
	}
	// Find C++ object associated with window.
	ListBoxX *lbx = static_cast<ListBoxX *>(PointerFromWindow(hWnd));
	if (lbx) {
		return lbx->WndProc(hWnd, iMessage, wParam, lParam);
	} else {
		return ::DefWindowProc(hWnd, iMessage, wParam, lParam);
	}
}

namespace {

bool ListBoxX_Register() noexcept {
	WNDCLASSEX wndclassc {};
	wndclassc.cbSize = sizeof(wndclassc);
	// We need CS_HREDRAW and CS_VREDRAW because of the ellipsis that might be drawn for
	// truncated items in the list and the appearance/disappearance of the vertical scroll bar.
	// The list repaint is double-buffered to avoid the flicker this would otherwise cause.
	wndclassc.style = CS_GLOBALCLASS | CS_HREDRAW | CS_VREDRAW;
	wndclassc.cbWndExtra = sizeof(LONG_PTR);
	wndclassc.hInstance = hinstPlatformRes;
	wndclassc.lpfnWndProc = ListBoxX::StaticWndProc;
	wndclassc.hCursor = ::LoadCursor({}, IDC_ARROW);
	wndclassc.lpszClassName = ListBoxX_ClassName;

	return ::RegisterClassEx(&wndclassc) != 0;
}

void ListBoxX_Unregister() noexcept {
	if (hinstPlatformRes) {
		::UnregisterClass(ListBoxX_ClassName, hinstPlatformRes);
	}
}

}

void Menu::CreatePopUp() noexcept {
	Destroy();
	mid = ::CreatePopupMenu();
}

void Menu::Destroy() noexcept {
	if (mid)
		::DestroyMenu(static_cast<HMENU>(mid));
	mid = nullptr;
}

void Menu::Show(Point pt, const Window &w) noexcept {
	::TrackPopupMenu(static_cast<HMENU>(mid),
		TPM_RIGHTBUTTON, static_cast<int>(pt.x - 4), static_cast<int>(pt.y), 0,
		HwndFromWindow(w), nullptr);
	Destroy();
}

ColourAlpha Platform::Chrome() noexcept {
	return ColourAlpha::FromRGB(::GetSysColor(COLOR_3DFACE));
}

ColourAlpha Platform::ChromeHighlight() noexcept {
	return ColourAlpha::FromRGB(::GetSysColor(COLOR_3DHIGHLIGHT));
}

const char *Platform::DefaultFont() noexcept {
	return "Verdana";
}

int Platform::DefaultFontSize() noexcept {
	return 10;
}

unsigned int Platform::DoubleClickTime() noexcept {
	return ::GetDoubleClickTime();
}

//#define TRACE

#ifdef TRACE
void Platform::DebugDisplay(const char *s) noexcept {
	::OutputDebugStringA(s);
}

void Platform::DebugPrintf(const char *format, ...) noexcept {
	char buffer[2000];
	va_list pArguments;
	va_start(pArguments, format);
	vsprintf(buffer, format, pArguments);
	va_end(pArguments);
	Platform::DebugDisplay(buffer);
}

static bool assertionPopUps = true;

bool Platform::ShowAssertionPopUps(bool assertionPopUps_) noexcept {
	const bool ret = assertionPopUps;
	assertionPopUps = assertionPopUps_;
	return ret;
}

void Platform::Assert(const char *c, const char *file, int line) noexcept {
	char buffer[2000]{};
	sprintf(buffer, "Assertion [%s] failed at %s %d%s", c, file, line, assertionPopUps ? "" : "\r\n");
	if (assertionPopUps) {
		const int idButton = ::MessageBoxA({}, buffer, "Assertion failure",
			MB_ABORTRETRYIGNORE | MB_ICONHAND | MB_SETFOREGROUND | MB_TASKMODAL);
		if (idButton == IDRETRY) {
			::DebugBreak();
		} else if (idButton == IDIGNORE) {
			// all OK
		} else {
			abort();
		}
	} else {
		Platform::DebugDisplay(buffer);
		::DebugBreak();
		abort();
	}
}

#else
void Platform::DebugDisplay(const char *) noexcept {
}

void Platform::DebugPrintf(const char *, ...) noexcept {
}

bool Platform::ShowAssertionPopUps(bool) noexcept {
	return false;
}

void Platform::Assert(const char *, const char *, int) noexcept {
}
#endif // TRACE

void Platform_Initialise(void *hInstance) noexcept {
	hinstPlatformRes = static_cast<HINSTANCE>(hInstance);
	//LoadDpiForWindow() is moved into wWinMain().
	ListBoxX_Register();
}

void Platform_Finalise(bool fromDllMain) noexcept {
#if defined(USE_D2D)
	if (!fromDllMain) {
		ReleaseUnknown(defaultRenderingParams);
		ReleaseUnknown(customClearTypeRenderingParams);
		ReleaseUnknown(gdiInterop);
		ReleaseUnknown(pIDWriteFactory);
		ReleaseUnknown(pD2DFactory);
		if (hDLLDWrite) {
			FreeLibrary(hDLLDWrite);
			hDLLDWrite = {};
		}
		if (hDLLD2D) {
			FreeLibrary(hDLLD2D);
			hDLLD2D = {};
		}
	}
#endif
#if !NP2_HAS_GETDPIFORWINDOW
	if (!fromDllMain && hShcoreDLL) {
		FreeLibrary(hShcoreDLL);
		hShcoreDLL = {};
	}
#endif
	ListBoxX_Unregister();
}

}
