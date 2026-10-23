#include "gui_fonts.hxx"

#include <cstddef>
#include <FL/Fl.H>
#include "gui.h"

#if _WIN32
#include <windows.h>
#include "spdlog/spdlog.h"
#elif __APPLE__
#include <CoreText/CoreText.h>
#include <cstring>
#else
#include <fontconfig/fontconfig.h>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unistd.h>
#endif

#if !_WIN32 && !__APPLE__
// Returns true if fontconfig already knows a face of the given family and
// weight, in which case the embedded copy does not need to be extracted.
static bool is_system_font_installed(const char *family, bool bold)
{
    FcPattern *pattern = FcPatternCreate();
    if (!pattern)
        return false;
    FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8 *>(family));
    FcPatternAddInteger(pattern, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
    FcObjectSet *object_set = FcObjectSetBuild(FC_FAMILY, FC_FILE, static_cast<char *>(NULL));
    FcFontSet *font_set = object_set ? FcFontList(NULL, pattern, object_set) : NULL;
    bool found = font_set && font_set->nfont > 0;
    if (font_set)
        FcFontSetDestroy(font_set);
    if (object_set)
        FcObjectSetDestroy(object_set);
    FcPatternDestroy(pattern);
    return found;
}
#endif

static const char *register_font_file(const char *font_family_name, const unsigned char *font_data, size_t font_size)
{

#if _WIN32
    // Windows: Use AddFontMemResourceEx to register the font from memory
    DWORD num_fonts = 0;
    HANDLE handle = AddFontMemResourceEx(const_cast<unsigned char *>(font_data), static_cast<DWORD>(font_size), NULL, &num_fonts);
    if (!handle || num_fonts == 0)
    {
        spdlog::error("Failed to register font: {}", font_family_name + 1);
        return NULL;
    }
    // FLTK on Windows expects the face name prefixed with an attribute character
    // (' ' = regular, 'B' = bold, 'I' = italic, 'P' = bold italic).
    return font_family_name;
#elif __APPLE__
    CFDataRef data = CFDataCreateWithBytesNoCopy(NULL, font_data, font_size, kCFAllocatorNull);
    if (!data)
        return NULL;
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CFRelease(data);
    if (!provider)
        return NULL;
    CGFontRef font = CGFontCreateWithDataProvider(provider);
    CGDataProviderRelease(provider);
    if (!font)
        return NULL;

    const char *result = NULL;
    CFErrorRef err = NULL;
    if (CTFontManagerRegisterGraphicsFont(font, &err))
    {
        CFStringRef name = CGFontCopyPostScriptName(font);
        char buf[256];
        if (name && CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8))
            result = strdup(buf);
        if (name)
            CFRelease(name);
    }
    else if (err)
    {
        CFRelease(err);
    }
    CGFontRelease(font);
    return result;
#else
    // FLTK expects the face name prefixed with an attribute character
    // (' ' = regular, 'B' = bold, 'I' = italic, 'P' = bold italic).
    if (is_system_font_installed(font_family_name + 1, font_family_name[0] == 'B'))
        return font_family_name;

    // Linux: Fontconfig cannot load fonts from memory, so write the font to a
    // temporary file and add it as an application font.
    static std::vector<std::string> temp_files;
    static bool cleanup_registered = false;
    if (!cleanup_registered)
    {
        cleanup_registered = true;
        std::atexit([]()
                    {
            for (const std::string &path : temp_files)
                unlink(path.c_str()); });
    }

    const char *tmpdir = getenv("TMPDIR");
    std::string path = std::string(tmpdir && *tmpdir ? tmpdir : "/tmp") + "/tableskim-font-XXXXXX";
    int fd = mkstemp(&path[0]);
    if (fd < 0)
        return NULL;
    FILE *fp = fdopen(fd, "wb");
    if (!fp)
    {
        close(fd);
        unlink(path.c_str());
        return NULL;
    }
    bool written = fwrite(font_data, 1, font_size, fp) == font_size;
    if (fclose(fp) != 0 || !written)
    {
        unlink(path.c_str());
        return NULL;
    }
    temp_files.push_back(path);

    if (!FcConfigAppFontAddFile(NULL, reinterpret_cast<const FcChar8 *>(path.c_str())))
        return NULL;
    return font_family_name;
#endif
}

#if !_WIN32 && !__APPLE__
// FLTK with Pango (its default on Linux) reads Pango names ("Sans Bold"),
// not the attribute prefix of its Xft driver ("Bsans"), which Pango would
// take as part of the family name. Its own bold Helvetica tells which.
static const char *fltk_font_name(const char *prefixed_name)
{
    static std::deque<std::string> names; // Fl::set_font() keeps the pointers
    const char *built_in = Fl::get_font(FL_HELVETICA_BOLD);
    if (!prefixed_name || !built_in || std::string(built_in).find(" Bold") == std::string::npos)
        return prefixed_name;
    names.push_back(std::string(prefixed_name + 1) + (prefixed_name[0] == 'B' ? " Bold" : ""));
    return names.back().c_str();
}
#else
static const char *fltk_font_name(const char *prefixed_name)
{
    return prefixed_name;
}
#endif

void register_fonts()
{
    // Register the fonts
    const char *helvetica_font =
        fltk_font_name(register_font_file(" BIZ UDPGothic", mplus1u_regular, sizeof(mplus1u_regular)));
    const char *helvetica_bold_font =
        fltk_font_name(register_font_file("BBIZ UDPGothic", mplus1u_bold, sizeof(mplus1u_bold)));
    const char *mono_font =
        fltk_font_name(register_font_file(" M PLUS 1 Code", mplus1_code_regular, sizeof(mplus1_code_regular)));
    const char *mono_bold_font =
        fltk_font_name(register_font_file("BM PLUS 1 Code", mplus1_code_bold, sizeof(mplus1_code_bold)));

    if (helvetica_font)
    {
        Fl::set_font(FL_HELVETICA, helvetica_font);
    }
    if (helvetica_bold_font)
    {
        Fl::set_font(FL_HELVETICA_BOLD, helvetica_bold_font);
    }
    if (mono_font)
    {
        Fl::set_font(FL_COURIER, mono_font);
    }
    if (mono_bold_font)
    {
        Fl::set_font(FL_COURIER_BOLD, mono_bold_font);
    }
}
