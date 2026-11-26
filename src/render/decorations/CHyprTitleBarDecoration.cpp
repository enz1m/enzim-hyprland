#include "CHyprTitleBarDecoration.hpp"
#include "../../config/ConfigValue.hpp"
#include "../../desktop/state/FocusState.hpp"
#include "../../desktop/state/ViewState.hpp"
#include "../../desktop/state/WindowState.hpp"
#include "../../desktop/view/window/Window.hpp"
#include "../../desktop/view/window/WindowPresentation.hpp"
#include "../../event/EventBus.hpp"
#include "../../ipc/s2/S2.hpp"
#include "../../layout/target/WindowTarget.hpp"
#include "../../layout/LayoutManager.hpp"
#include "../../layout/supplementary/DragController.hpp"
#include "../../managers/SessionLockManager.hpp"
#include "../../managers/fullscreen/FullscreenController.hpp"
#include "../../output/Monitor.hpp"
#include "../Renderer.hpp"
#include "../pass/RectPassElement.hpp"
#include "../pass/TexPassElement.hpp"
#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <ranges>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>

using namespace Render;

std::map<CHyprTitleBarDecoration::Contact, CHyprTitleBarDecoration::SContact> CHyprTitleBarDecoration::s_contacts;
std::optional<CHyprTitleBarDecoration::Contact>                               CHyprTitleBarDecoration::s_dragContact;
CHyprSignalListener                                                           CHyprTitleBarDecoration::s_dragEnded;

static constexpr double                                                       DEFAULT_DRAG_THRESHOLD = 4;

// A cluster of several controls shares one pill whose padding each control gives up.
static constexpr double PILL_PADDING = 2;
// A control in a shared pill highlights a circle this far inside its target.
static constexpr double PILL_HOVER_INSET = 1;
// In a shared pill the hover fill steps further from the resting fill than a lone control's.
static constexpr double PILL_HOVER_EMPHASIS = 1.6;
// The frame glyphs fill their whole box, so they draw smaller to match the close cross.
static constexpr double FRAME_GLYPH_SHRINK = 2;
static constexpr double TITLE_GAP          = 8;
// A press this close to the bar's top edge resizes the window instead of moving it.
static constexpr double RESIZE_GRAB   = 4;
static constexpr double RESIZE_CORNER = 12;
// Matches the GTK and Qt default double-click interval and distance.
static constexpr uint32_t                               DOUBLE_CLICK_MS       = 400;
static constexpr double                                 DOUBLE_CLICK_DISTANCE = 6;

static std::optional<CHyprTitleBarDecoration::eControl> controlNamed(std::string_view name) {
    if (name == "minimize")
        return CHyprTitleBarDecoration::CONTROL_MINIMIZE;
    if (name == "maximize")
        return CHyprTitleBarDecoration::CONTROL_MAXIMIZE;
    if (name == "close")
        return CHyprTitleBarDecoration::CONTROL_CLOSE;
    return std::nullopt;
}

static std::vector<CHyprTitleBarDecoration::eControl> controlsIn(std::string_view list) {
    std::vector<CHyprTitleBarDecoration::eControl> controls;
    for (const auto part : list | std::views::split(',')) {
        const auto CONTROL = controlNamed(std::string_view{part.begin(), part.end()});
        if (CONTROL && std::ranges::find(controls, *CONTROL) == controls.end())
            controls.push_back(*CONTROL);
    }
    return controls;
}

static CHyprColor colorOr(int64_t value, int64_t fallback) {
    return CHyprColor(sc<uint64_t>(value == -1 ? fallback : value));
}

static CHyprColor stepped(const CHyprColor& from, const CHyprColor& to, double factor) {
    const auto step = [factor](double a, double b) { return sc<float>(std::clamp(a + (b - a) * factor, 0.0, 1.0)); };
    return CHyprColor(step(from.r, to.r), step(from.g, to.g), step(from.b, to.b), sc<float>(to.a));
}

static void setSource(cairo_t* cairo, const CHyprColor& color) {
    cairo_set_source_rgba(cairo, color.r, color.g, color.b, color.a);
}

// Draws the Lucide glyph the enzUI window controls use, in its 24-unit view box.
static void drawGlyph(cairo_t* cairo, CHyprTitleBarDecoration::eControl control, bool maximized) {
    static constexpr double PI = std::numbers::pi;
    switch (control) {
        case CHyprTitleBarDecoration::CONTROL_CLOSE:
            cairo_move_to(cairo, 18, 6);
            cairo_line_to(cairo, 6, 18);
            cairo_move_to(cairo, 6, 6);
            cairo_line_to(cairo, 18, 18);
            break;
        case CHyprTitleBarDecoration::CONTROL_MINIMIZE:
            cairo_move_to(cairo, 5, 12);
            cairo_line_to(cairo, 19, 12);
            break;
        case CHyprTitleBarDecoration::CONTROL_MAXIMIZE:
            if (maximized) {
                cairo_move_to(cairo, 8, 3);
                cairo_line_to(cairo, 8, 6);
                cairo_arc(cairo, 6, 6, 2, 0, PI / 2);
                cairo_line_to(cairo, 3, 8);
                cairo_new_sub_path(cairo);
                cairo_move_to(cairo, 21, 8);
                cairo_line_to(cairo, 18, 8);
                cairo_arc(cairo, 18, 6, 2, PI / 2, PI);
                cairo_line_to(cairo, 16, 3);
                cairo_new_sub_path(cairo);
                cairo_move_to(cairo, 3, 16);
                cairo_line_to(cairo, 6, 16);
                cairo_arc(cairo, 6, 18, 2, -PI / 2, 0);
                cairo_line_to(cairo, 8, 21);
                cairo_new_sub_path(cairo);
                cairo_move_to(cairo, 16, 21);
                cairo_line_to(cairo, 16, 18);
                cairo_arc(cairo, 18, 18, 2, PI, PI * 1.5);
                cairo_line_to(cairo, 21, 16);
            } else {
                cairo_move_to(cairo, 8, 3);
                cairo_line_to(cairo, 5, 3);
                cairo_arc_negative(cairo, 5, 5, 2, PI * 1.5, PI);
                cairo_line_to(cairo, 3, 8);
                cairo_new_sub_path(cairo);
                cairo_move_to(cairo, 21, 8);
                cairo_line_to(cairo, 21, 5);
                cairo_arc_negative(cairo, 19, 5, 2, 0, -PI / 2);
                cairo_line_to(cairo, 16, 3);
                cairo_new_sub_path(cairo);
                cairo_move_to(cairo, 3, 16);
                cairo_line_to(cairo, 3, 19);
                cairo_arc_negative(cairo, 5, 19, 2, PI, PI / 2);
                cairo_line_to(cairo, 8, 21);
                cairo_new_sub_path(cairo);
                cairo_move_to(cairo, 16, 21);
                cairo_line_to(cairo, 19, 21);
                cairo_arc_negative(cairo, 19, 19, 2, PI / 2, 0);
                cairo_line_to(cairo, 21, 16);
            }
            break;
    }
}

static SP<ITexture> textureFrom(cairo_surface_t* surface) {
    cairo_surface_flush(surface);
    auto texture = g_pHyprRenderer->createTexture(surface);
    cairo_surface_destroy(surface);
    return texture;
}
CHyprTitleBarDecoration::SButtonLayout CHyprTitleBarDecoration::parseButtonLayout(std::string_view layout) {
    const auto COLON = layout.find(':');
    if (COLON == std::string_view::npos)
        return {.trailing = controlsIn(layout)};

    SButtonLayout result = {.leading = controlsIn(layout.substr(0, COLON)), .trailing = controlsIn(layout.substr(COLON + 1))};
    std::erase_if(result.trailing, [&](eControl control) { return std::ranges::find(result.leading, control) != result.leading.end(); });
    return result;
}

CHyprTitleBarDecoration::~CHyprTitleBarDecoration() = default;

CHyprTitleBarDecoration::CHyprTitleBarDecoration(PHLWINDOW pWindow) : IHyprWindowDecoration(pWindow) {
    m_mouseMove      = Event::bus()->m_events.input.mouse.move.listen([this](Vector2D pos, Event::SCallbackInfo&) { onPointerMove(pos); });
    m_propsRefreshed = Event::bus()->m_events.config.props_refreshed.listen([this](bool) { damageEntire(); });
    m_titleChanged   = Event::bus()->m_events.window.title.listen([this](PHLWINDOW window) {
        if (window == owningWindow())
            damageEntire();
    });
}

bool CHyprTitleBarDecoration::wanted() const {
    static auto PENABLED = CConfigValue<Config::INTEGER>("decoration:titlebar:enabled");

    const auto  WINDOW = owningWindow();
    if (!*PENABLED || !WINDOW || !WINDOW->mapped())
        return false;

    if (!WINDOW->m_ruleApplicator->decorate().valueOrDefault())
        return false;

    if (Fullscreen::controller()->getFullscreenModes(WINDOW).internal == Fullscreen::FSMODE_FULLSCREEN)
        return false;

    return WINDOW->backend().wantsServerDecorations();
}

bool CHyprTitleBarDecoration::shown() const {
    return m_shown;
}

void CHyprTitleBarDecoration::refresh() {
    const bool WANTED = wanted();
    if (WANTED == m_shown)
        return;

    const auto WINDOW = owningWindow();
    // The border and shadow grow or shrink with the bar, so the whole window is damaged.
    if (WINDOW)
        g_pHyprRenderer->damageWindow(WINDOW);

    m_shown   = WANTED;
    m_hovered = std::nullopt;
    m_lastTap.reset();
    g_pDecorationPositioner->repositionDeco(this);

    if (WINDOW && WINDOW->layoutTarget())
        WINDOW->layoutTarget()->recalc();

    if (WINDOW)
        g_pHyprRenderer->damageWindow(WINDOW);
}

SDecorationPositioningInfo CHyprTitleBarDecoration::getPositioningInfo() {
    static auto                PHEIGHT = CConfigValue<Config::INTEGER>("decoration:titlebar:height");

    SDecorationPositioningInfo info;
    info.policy = DECORATION_POSITION_STICKY;
    info.edges  = DECORATION_EDGE_TOP;
    // Above the border's priority, so the border frames the bar and the window together.
    info.priority       = 10001;
    info.reserved       = true;
    info.desiredExtents = {Vector2D{0.0, m_shown ? sc<double>(*PHEIGHT) : 0.0}, Vector2D{}};
    return info;
}

void CHyprTitleBarDecoration::onPositioningReply(const SDecorationPositioningReply& reply) {
    m_assignedBox = reply.assignedGeometry;
}

eDecorationType CHyprTitleBarDecoration::getDecorationType() {
    return DECORATION_TITLEBAR;
}

void CHyprTitleBarDecoration::updateWindow(PHLWINDOW) {
    refresh();
}

void CHyprTitleBarDecoration::updateState() {
    refresh();
    damageEntire();
}

void CHyprTitleBarDecoration::onWindowMap() {
    refresh();
}

eDecorationLayer CHyprTitleBarDecoration::getDecorationLayer() {
    return DECORATION_LAYER_UNDER;
}

uint64_t CHyprTitleBarDecoration::getDecorationFlags() {
    return DECORATION_PART_OF_MAIN_WINDOW;
}

std::string CHyprTitleBarDecoration::getDisplayName() {
    return "TitleBar";
}

CBox CHyprTitleBarDecoration::barBoxGlobal() const {
    const auto WINDOW = owningWindow();
    if (!WINDOW)
        return {};

    CBox box = m_assignedBox;
    box.translate(g_pDecorationPositioner->getEdgeDefinedPoint(DECORATION_EDGE_TOP, WINDOW));

    if (WINDOW->m_workspace && !(WINDOW->m_state & Desktop::View::WINDOW_STATE_PINNED))
        box.translate(WINDOW->m_workspace->m_renderOffset->value());

    return box;
}

void CHyprTitleBarDecoration::damageEntire() {
    const auto WINDOW = owningWindow();
    if (!WINDOW)
        return;

    auto box = barBoxGlobal();
    if (box.empty())
        return;

    box.translate(WINDOW->presentation().floatingOffset());
    g_pHyprRenderer->damageBox(box.expand(2));
}

CHyprTitleBarDecoration::SLayout CHyprTitleBarDecoration::layout(double width) const {
    static auto PHEIGHT     = CConfigValue<Config::INTEGER>("decoration:titlebar:height");
    static auto PPADDING    = CConfigValue<Config::INTEGER>("decoration:titlebar:padding");
    static auto PBUTTONSIZE = CConfigValue<Config::INTEGER>("decoration:titlebar:button_size");
    static auto PBUTTONS    = CConfigValue<std::string>("decoration:titlebar:buttons");

    const auto  WINDOW = owningWindow();
    SLayout     result;
    if (!WINDOW)
        return result;

    const auto offers = [&](eControl control) { return this->offers(WINDOW, control); };

    auto [leading, trailing] = parseButtonLayout(*PBUTTONS);
    std::erase_if(leading, [&](eControl control) { return !offers(control); });
    std::erase_if(trailing, [&](eControl control) { return !offers(control); });
    // A window the compositor frames must stay closable.
    if (std::ranges::find(leading, CONTROL_CLOSE) == leading.end() && std::ranges::find(trailing, CONTROL_CLOSE) == trailing.end())
        trailing.push_back(CONTROL_CLOSE);

    const double HEIGHT = *PHEIGHT;
    const double SIZE   = std::min<double>(*PBUTTONSIZE, HEIGHT);
    const double INSET  = (HEIGHT - SIZE) / 2.0;

    const auto   place = [&](const std::vector<eControl>& controls, double x) {
        const double CONTROL = controls.size() > 1 ? SIZE - 2 * PILL_PADDING : SIZE;
        const double OFFSET  = controls.size() > 1 ? PILL_PADDING : 0;
        for (size_t i = 0; i < controls.size(); ++i)
            result.controls.push_back({controls[i], {x + OFFSET + i * CONTROL, INSET + OFFSET, CONTROL, CONTROL}});
        return CBox{x, INSET, controls.size() > 1 ? controls.size() * CONTROL + 2 * PILL_PADDING : SIZE, SIZE};
    };
    const auto clusterWidth = [&](const std::vector<eControl>& controls) {
        return controls.empty() ? 0.0 : controls.size() > 1 ? controls.size() * (SIZE - 2 * PILL_PADDING) + 2 * PILL_PADDING : SIZE;
    };

    if (!leading.empty())
        result.leading = place(leading, INSET);
    if (!trailing.empty())
        result.trailing = place(trailing, width - INSET - clusterWidth(trailing));

    const double START = leading.empty() ? *PPADDING : result.leading.x + result.leading.w + TITLE_GAP;
    const double END   = trailing.empty() ? width - *PPADDING : result.trailing.x - TITLE_GAP;
    result.titleX      = START;
    result.titleW      = std::max(0.0, END - START);
    return result;
}

void CHyprTitleBarDecoration::draw(PHLMONITOR pMonitor, float const& a) {
    static auto PACTIVE   = CConfigValue<Config::INTEGER>("decoration:titlebar:col.active");
    static auto PINACTIVE = CConfigValue<Config::INTEGER>("decoration:titlebar:col.inactive");

    const auto  WINDOW = owningWindow();
    if (!m_shown || !WINDOW || m_assignedBox.empty())
        return;

    const bool FOCUSED = Desktop::focusState()->window() == WINDOW;
    auto       barPx   = barBoxGlobal().translate(-pMonitor->m_position + WINDOW->presentation().floatingOffset()).scale(pMonitor->m_scale).round();
    if (barPx.w < 1 || barPx.h < 1)
        return;

    CHyprColor background = FOCUSED ? CHyprColor(sc<uint64_t>(*PACTIVE)) : colorOr(*PINACTIVE, *PACTIVE);
    background.a *= a;

    // Extend the rounded shape past the clip to square its bottom corners.
    const double                ROUNDING = WINDOW->presentation().rounding() * pMonitor->m_scale;
    const double                OVERLAP  = std::ceil(ROUNDING) + 1;

    CRectPassElement::SRectData rect;
    rect.color         = background;
    rect.box           = {barPx.x, barPx.y, barPx.w, barPx.h + OVERLAP};
    rect.clipBox       = barPx;
    rect.round         = sc<int>(std::round(ROUNDING));
    rect.roundingPower = WINDOW->presentation().roundingPower();
    g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(rect));

    const auto LAYOUT = layout(m_assignedBox.w);
    auto&      cache  = m_caches[pMonitor->m_scale];
    drawTitle(pMonitor, LAYOUT, barPx, a);
    if (!LAYOUT.leading.empty())
        drawCluster(pMonitor, LAYOUT, LAYOUT.leading, barPx, a, cache.leading);
    if (!LAYOUT.trailing.empty())
        drawCluster(pMonitor, LAYOUT, LAYOUT.trailing, barPx, a, cache.trailing);
}

void CHyprTitleBarDecoration::drawTitle(PHLMONITOR pMonitor, const SLayout& layout, const CBox& barPx, float a) {
    static auto  PFONT         = CConfigValue<std::string>("decoration:titlebar:font_family");
    static auto  PFALLBACKFONT = CConfigValue<std::string>("misc:font_family");
    static auto  PFONTSIZE     = CConfigValue<Config::FLOAT>("decoration:titlebar:font_size");
    static auto  PFONTWEIGHT   = CConfigValue<Config::INTEGER>("decoration:titlebar:font_weight");
    static auto  PTEXT         = CConfigValue<Config::INTEGER>("decoration:titlebar:col.text");
    static auto  PTEXTINACTIVE = CConfigValue<Config::INTEGER>("decoration:titlebar:col.text_inactive");

    const auto   WINDOW = owningWindow();
    const auto&  TITLE  = WINDOW->metadata().title();
    const double SCALE  = pMonitor->m_scale;
    const int    WIDTH  = sc<int>(std::floor(layout.titleW * SCALE));
    if (TITLE.empty() || WIDTH < 1)
        return;

    const bool        FOCUSED = Desktop::focusState()->window() == WINDOW;
    const CHyprColor  COLOR   = FOCUSED ? CHyprColor(sc<uint64_t>(*PTEXT)) : colorOr(*PTEXTINACTIVE, *PTEXT);
    const std::string FONT    = *PFONT != STRVAL_EMPTY && !(*PFONT).empty() ? *PFONT : *PFALLBACKFONT;
    const auto        KEY     = std::format("{}|{}|{}|{}|{}|{}|{:x}", TITLE, SCALE, WIDTH, FONT, *PFONTSIZE, *PFONTWEIGHT, COLOR.getAsHex());

    auto&             cache = m_caches[pMonitor->m_scale].title;
    if (cache.key != KEY || !cache.texture) {
        PangoFontMap*         fontMap = pango_cairo_font_map_get_default();
        PangoContext*         context = pango_font_map_create_context(fontMap);
        cairo_font_options_t* options = cairo_font_options_create();
        // Fractional scales place glyphs off the pixel grid, so hinting would distort their spacing.
        cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
        cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
        pango_cairo_context_set_font_options(context, options);
        cairo_font_options_destroy(options);

        PangoLayout*          text = pango_layout_new(context);
        PangoFontDescription* desc = pango_font_description_new();
        pango_font_description_set_family(desc, FONT.c_str());
        pango_font_description_set_absolute_size(desc, *PFONTSIZE * SCALE * PANGO_SCALE);
        pango_font_description_set_weight(desc, sc<PangoWeight>(*PFONTWEIGHT));
        pango_layout_set_font_description(text, desc);
        pango_layout_set_text(text, TITLE.c_str(), -1);
        pango_layout_set_single_paragraph_mode(text, true);
        pango_layout_set_width(text, WIDTH * PANGO_SCALE);
        pango_layout_set_ellipsize(text, PANGO_ELLIPSIZE_END);

        PangoRectangle logical = {};
        pango_layout_get_pixel_extents(text, nullptr, &logical);
        const int TEXTW = std::clamp(logical.x + logical.width, 1, WIDTH);
        const int TEXTH = std::max(logical.height, 1);

        auto*     surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, TEXTW, TEXTH);
        auto*     cairo   = cairo_create(surface);
        setSource(cairo, COLOR);
        pango_cairo_show_layout(cairo, text);
        cairo_destroy(cairo);

        pango_font_description_free(desc);
        g_object_unref(text);
        g_object_unref(context);

        cache = {KEY, textureFrom(surface)};
    }

    const auto& TEXTURE = cache.texture;
    if (!TEXTURE)
        return;

    CTexPassElement::SRenderData data;
    data.tex = TEXTURE;
    data.box = {std::round(barPx.x + layout.titleX * SCALE), std::round(barPx.y + (barPx.h - TEXTURE->m_size.y) / 2.0), TEXTURE->m_size.x, TEXTURE->m_size.y};
    data.a   = a;
    g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(std::move(data)));
}

void CHyprTitleBarDecoration::drawCluster(PHLMONITOR pMonitor, const SLayout& layout, const CBox& cluster, const CBox& barPx, float a, STextureCache& cache) {
    static auto         PGLYPHSIZE   = CConfigValue<Config::INTEGER>("decoration:titlebar:glyph_size");
    static auto         PBUTTON      = CConfigValue<Config::INTEGER>("decoration:titlebar:col.button");
    static auto         PBUTTONHOVER = CConfigValue<Config::INTEGER>("decoration:titlebar:col.button_hover");
    static auto         PGLYPH       = CConfigValue<Config::INTEGER>("decoration:titlebar:col.glyph");
    static auto         PGLYPHHOVER  = CConfigValue<Config::INTEGER>("decoration:titlebar:col.glyph_hover");

    const auto          WINDOW    = owningWindow();
    const double        SCALE     = pMonitor->m_scale;
    const bool          MAXIMIZED = Fullscreen::controller()->isFullscreen(WINDOW, Fullscreen::FSMODE_MAXIMIZED);

    std::vector<size_t> members;
    for (size_t i = 0; i < layout.controls.size(); ++i) {
        if (cluster.containsPoint(layout.controls[i].box.middle()))
            members.push_back(i);
    }
    if (members.empty())
        return;

    const auto  ACTIVE = m_hovered;
    const bool  PILL   = members.size() > 1;
    const auto  PX     = CBox{std::round(barPx.x + cluster.x * SCALE), std::round(barPx.y + cluster.y * SCALE), std::round(cluster.w * SCALE), std::round(cluster.h * SCALE)};

    std::string key = std::format("{}|{}|{}|{}|{}|{}|{}|{}|{}", SCALE, PX.w, PX.h, *PGLYPHSIZE, *PBUTTON, *PBUTTONHOVER, *PGLYPH, *PGLYPHHOVER, MAXIMIZED);
    for (const auto i : members)
        key += std::format("|{}{}", sc<int>(layout.controls[i].kind), ACTIVE == layout.controls[i].kind ? "*" : "");

    if (cache.key != key || !cache.texture) {
        const auto BUTTON      = CHyprColor(sc<uint64_t>(*PBUTTON));
        const auto BUTTONHOVER = CHyprColor(sc<uint64_t>(*PBUTTONHOVER));
        const auto GLYPH       = CHyprColor(sc<uint64_t>(*PGLYPH));
        const auto GLYPHHOVER  = CHyprColor(sc<uint64_t>(*PGLYPHHOVER));

        auto*      surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, PX.w, PX.h);
        auto*      cairo   = cairo_create(surface);
        cairo_scale(cairo, PX.w / cluster.w, PX.h / cluster.h);

        const double RADIUS = cluster.h / 2.0;
        if (PILL) {
            cairo_new_sub_path(cairo);
            cairo_arc(cairo, RADIUS, RADIUS, RADIUS, std::numbers::pi / 2, std::numbers::pi * 1.5);
            cairo_arc(cairo, cluster.w - RADIUS, RADIUS, RADIUS, -std::numbers::pi / 2, std::numbers::pi / 2);
            cairo_close_path(cairo);
            setSource(cairo, BUTTON);
            cairo_fill(cairo);
        }

        for (const auto i : members) {
            const auto&  CONTROL = layout.controls[i];
            const CBox   LOCAL   = CONTROL.box.copy().translate(-cluster.pos());
            const bool   HOT     = ACTIVE == CONTROL.kind;
            const double INSET   = PILL ? PILL_HOVER_INSET : 0;
            if (!PILL || HOT) {
                cairo_new_sub_path(cairo);
                cairo_arc(cairo, LOCAL.middle().x, LOCAL.middle().y, LOCAL.w / 2.0 - INSET, 0, std::numbers::pi * 2);
                setSource(cairo, HOT ? (PILL ? stepped(BUTTON, BUTTONHOVER, PILL_HOVER_EMPHASIS) : BUTTONHOVER) : BUTTON);
                cairo_fill(cairo);
            }

            const double GLYPHSIZE = *PGLYPHSIZE - (CONTROL.kind == CONTROL_MAXIMIZE ? FRAME_GLYPH_SHRINK : 0);
            cairo_save(cairo);
            cairo_translate(cairo, LOCAL.middle().x - GLYPHSIZE / 2.0, LOCAL.middle().y - GLYPHSIZE / 2.0);
            cairo_scale(cairo, GLYPHSIZE / 24.0, GLYPHSIZE / 24.0);
            drawGlyph(cairo, CONTROL.kind, MAXIMIZED);
            cairo_restore(cairo);
            cairo_set_line_width(cairo, 2.0 * GLYPHSIZE / 24.0);
            cairo_set_line_cap(cairo, CAIRO_LINE_CAP_ROUND);
            cairo_set_line_join(cairo, CAIRO_LINE_JOIN_ROUND);
            setSource(cairo, HOT ? GLYPHHOVER : GLYPH);
            cairo_stroke(cairo);
        }

        cairo_destroy(cairo);
        cache = {key, textureFrom(surface)};
    }

    if (!cache.texture)
        return;

    CTexPassElement::SRenderData data;
    data.tex = cache.texture;
    data.box = PX;
    data.a   = a;
    g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(std::move(data)));
}

std::optional<CHyprTitleBarDecoration::eControl> CHyprTitleBarDecoration::controlAt(const Vector2D& global) const {
    const auto WINDOW = owningWindow();
    if (!m_shown || !WINDOW)
        return std::nullopt;

    const auto BAR    = barBoxGlobal();
    const auto LAYOUT = layout(BAR.w);
    const auto LOCAL  = global - BAR.pos();
    for (size_t i = 0; i < LAYOUT.controls.size(); ++i) {
        if (LAYOUT.controls[i].box.containsPoint(LOCAL))
            return LAYOUT.controls[i].kind;
    }
    return std::nullopt;
}

void CHyprTitleBarDecoration::onPointerMove(const Vector2D& global) {
    if (!m_shown)
        return;

    std::optional<eControl> hovered;
    if (const auto WINDOW = owningWindow(); WINDOW && barBoxGlobal().containsPoint(global) &&
        Desktop::viewState()->hitTest().windowAt(global, Desktop::View::ALLOW_FLOATING | Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS) == WINDOW)
        hovered = controlAt(global);

    if (hovered == m_hovered)
        return;

    m_hovered = hovered;
    damageEntire();
}

bool CHyprTitleBarDecoration::acceptsInputAt(const Vector2D& pos) const {
    const auto WINDOW = owningWindow();
    return m_shown && validMapped(WINDOW) && !WINDOW->isHidden() && barBoxGlobal().containsPoint(pos) && !WINDOW->hasPopupAt(pos);
}

bool CHyprTitleBarDecoration::offers(PHLWINDOW window, eControl control) {
    const auto TRAITS = window->backend().traits();
    const auto HINTS  = window->backend().geometryHints(Desktop::View::eBackendState::BACKEND_STATE_CURRENT);
    const bool FIXED  = HINTS.minSize && HINTS.maxSize && HINTS.minSize->x > 0 && HINTS.minSize->y > 0 && *HINTS.minSize == *HINTS.maxSize;
    const bool DIALOG = TRAITS.modal || TRAITS.transient;
    switch (control) {
        case CONTROL_MINIMIZE: return !DIALOG;
        case CONTROL_MAXIMIZE: return !DIALOG && !FIXED;
        case CONTROL_CLOSE: return true;
    }
    return false;
}

bool CHyprTitleBarDecoration::beginContact(Contact contact, PHLWINDOW window, const Vector2D& pos, uint32_t time, bool primary, PHLMONITOR monitor) {
    endContact(contact, std::nullopt, time, true);
    if (!window || g_pSessionLockManager->isSessionLocked())
        return false;

    const auto BAR = window->presentation().titleBarAt(pos);
    if (!BAR)
        return false;

    s_contacts.emplace(contact, SContact{.owner = BAR, .monitor = monitor, .start = pos, .pos = pos, .control = BAR->controlAt(pos), .primary = primary});
    if (primary) {
        if (!Desktop::focusState()->isWindowActive(window))
            Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_CLICK_DOWN);
        if (window->isFloating())
            Desktop::windowState()->raise(window);
    }
    return true;
}

bool CHyprTitleBarDecoration::moveTouchContact(Contact contact, const Vector2D& normalizedPos) {
    const auto IT = s_contacts.find(contact);
    if (IT == s_contacts.end())
        return false;

    if (const auto MONITOR = IT->second.monitor.lock())
        moveContact(contact, MONITOR->m_position + normalizedPos * MONITOR->m_size);
    return true;
}

bool CHyprTitleBarDecoration::moveContact(Contact contact, const Vector2D& pos) {
    const auto IT = s_contacts.find(contact);
    if (IT == s_contacts.end())
        return false;

    auto&      grab = IT->second;
    const auto BAR  = grab.owner.lock();
    grab.pos        = pos;
    if (!BAR || !BAR->shown() || !validMapped(BAR->owningWindow()) || BAR->owningWindow()->isHidden() || g_pSessionLockManager->isSessionLocked()) {
        grab.moved = true;
        if (s_dragContact == contact)
            g_layoutManager->endDragTarget();
        return true;
    }

    auto* const DRAG = g_layoutManager->dragController().get();
    if (s_dragContact == contact) {
        DRAG->mouseMove(pos);
        return true;
    }

    static auto  PTHRESHOLD = CConfigValue<Config::INTEGER>("binds:drag_threshold");
    const double THRESHOLD  = *PTHRESHOLD > 0 ? *PTHRESHOLD : DEFAULT_DRAG_THRESHOLD;
    if (!grab.primary || grab.control || grab.moved || grab.start.distanceSq(pos) <= THRESHOLD * THRESHOLD)
        return true;

    grab.moved = true;
    BAR->m_lastTap.reset();
    const auto WINDOW = BAR->owningWindow();
    if (!validMapped(WINDOW) || WINDOW->isHidden() || DRAG->target() || WINDOW->m_ruleApplicator->noXdgDrags().valueOrDefault())
        return true;

    const auto                         BOX   = BAR->barBoxGlobal();
    const auto                         LOCAL = grab.start - BOX.pos();
    std::optional<Layout::eRectCorner> edge;
    if (LOCAL.y < RESIZE_GRAB && WINDOW->isFloating() && !Fullscreen::controller()->isFullscreen(WINDOW))
        edge = LOCAL.x < RESIZE_CORNER ? Layout::CORNER_TOPLEFT : LOCAL.x > BOX.w - RESIZE_CORNER ? Layout::CORNER_TOPRIGHT : Layout::CORNER_TOP;

    const auto START = Fullscreen::controller()->isFullscreen(WINDOW) || !WINDOW->isFloating() ? pos : grab.start;
    DRAG->dragBegin(WINDOW->layoutTarget(), edge ? MBIND_RESIZE : MBIND_MOVE, edge, true, START);
    if (DRAG->target() == WINDOW->layoutTarget()) {
        s_dragContact = contact;
        s_dragEnded   = DRAG->m_events.ended.listen([] { s_dragContact.reset(); });
        DRAG->mouseMove(pos);
    }
    return true;
}

bool CHyprTitleBarDecoration::endContact(Contact contact, std::optional<Vector2D> pos, uint32_t time, bool cancelled) {
    auto entry = s_contacts.extract(contact);
    if (entry.empty())
        return false;

    if (s_dragContact == contact)
        g_layoutManager->endDragTarget();

    const auto& GRAB = entry.mapped();
    const auto  BAR  = GRAB.owner.lock();
    if (!BAR || !GRAB.primary || GRAB.moved)
        return true;

    if (cancelled || g_pSessionLockManager->isSessionLocked() || !BAR->acceptsInputAt(pos.value_or(GRAB.pos))) {
        BAR->m_lastTap.reset();
        return true;
    }

    const auto RELEASE = pos.value_or(GRAB.pos);
    const auto CONTROL = BAR->controlAt(RELEASE);
    if (GRAB.control || CONTROL) {
        BAR->m_lastTap.reset();
        if (GRAB.control && GRAB.control == CONTROL && offers(BAR->owningWindow(), *CONTROL))
            BAR->activate(*CONTROL);
        return true;
    }

    const auto PREVIOUS = std::exchange(BAR->m_lastTap, std::nullopt);
    if (GRAB.start.distance(RELEASE) > DOUBLE_CLICK_DISTANCE)
        return true;
    if (PREVIOUS && PREVIOUS->device == contact.first && time - PREVIOUS->time <= DOUBLE_CLICK_MS && PREVIOUS->pos.distance(RELEASE) <= DOUBLE_CLICK_DISTANCE) {
        if (offers(BAR->owningWindow(), CONTROL_MAXIMIZE))
            BAR->activate(CONTROL_MAXIMIZE);
    } else
        BAR->m_lastTap = STap{contact.first, time, RELEASE};
    return true;
}

void CHyprTitleBarDecoration::cancelContacts(const IHID* device) {
    if (s_dragContact && s_dragContact->first == device)
        g_layoutManager->endDragTarget();
    std::erase_if(s_contacts, [device](const auto& item) {
        if (item.first.first != device)
            return false;
        if (const auto BAR = item.second.owner.lock())
            BAR->m_lastTap.reset();
        return true;
    });
}

bool CHyprTitleBarDecoration::ownsContact(Contact contact) {
    return s_contacts.contains(contact);
}

bool CHyprTitleBarDecoration::dragging() {
    return s_dragContact.has_value();
}

void CHyprTitleBarDecoration::activate(eControl control) {
    const auto WINDOW = owningWindow();
    if (!WINDOW || !WINDOW->mapped())
        return;

    switch (control) {
        case CONTROL_CLOSE: WINDOW->sendClose(); break;
        case CONTROL_MAXIMIZE:
            WINDOW->requestClientFullscreen({
                .maximized = !Fullscreen::controller()->isFullscreen(WINDOW, Fullscreen::FSMODE_MAXIMIZED),
                .origin    = Desktop::View::SClientFullscreenRequest::ORIGIN_FOREIGN_TOPLEVEL,
            });
            break;
        case CONTROL_MINIMIZE:
            IPC::Socket2::sock()->postEvent({.event = "minimized", .data = std::format("{:x},1", rc<uintptr_t>(WINDOW.get()))});
            Event::bus()->m_events.window.minimize.emit(WINDOW, true);
            break;
    }
}
