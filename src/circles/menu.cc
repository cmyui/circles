#include <circles/app.h>

#include <cmath>
#include <random>
#include <string>

namespace circles {

namespace {

constexpr f32 kTau = 6.2831853f;
constexpr i32 kBars = 128;  // visualizer bars around the logo
constexpr i32 kParticles = 36;

std::mt19937& random_engine() {
  static std::mt19937 engine{std::random_device{}()};
  return engine;
}

f32 random_between(f32 low, f32 high) {
  return std::uniform_real_distribution<f32>(low, high)(random_engine());
}

Particle spawn_particle(const App& app, f32 y) {
  return {random_between(0, app.width), y, random_between(20, 110),
          random_between(10, 40), random_between(0.02f, 0.08f)};
}

struct Layout {
  Vec2 logo;
  f32  logo_radius;
  f32  button_x;
  f32  button_y[2];
  f32  button_w;
  f32  button_h;
};

Layout layout(const App& app) {
  const f32     radius = app.height * 0.21f;
  const Vec2    logo{app.width * 0.4f, app.height * 0.5f};
  constexpr f32 kWidth = 280, kHeight = 64;
  return {logo,
          radius,
          logo.x + radius + 60,
          {logo.y - kHeight - 10, logo.y + 10},
          kWidth,
          kHeight};
}

// 0 for the logo, 1 for play, 2 for exit, or -1.
i32 item_at(const App& app, Vec2 point) {
  const Layout l = layout(app);
  if ((point - l.logo).length() <= l.logo_radius)
    return 0;
  for (i32 i = 0; i < 2; ++i)
    if (contains(point, l.button_x, l.button_y[i], l.button_w, l.button_h))
      return i + 1;
  return -1;
}

void activate(App& app, i32 item) {
  if (item < 0)
    return;
  play_sound(app.audio, Sound::MenuClick, 0.7f);
  if (item == 2)
    app.quit = true;
  else
    switch_screen(app, Screen::SongSelect);
}

void play_random_song(App& app) {
  const auto& sets = app.library.sets;
  if (sets.empty())
    return;
  const i32 current =
      app.selected_map >= 0 ? app.library.set_of_map[app.selected_map] : -1;
  i32 set = std::uniform_int_distribution<i32>(
      0, static_cast<i32>(sets.size()) - 1)(random_engine());
  if (set == current && sets.size() > 1)
    set = (set + 1) % static_cast<i32>(sets.size());
  select_map(app, sets[set].maps.back(), false);
  play_preview(app);
}

// The beat being heard, so it allows for the audio offset.
Beat current_beat(const App& app) {
  if (app.selected_map >= 0 && app.audio.music_loaded)
    return beat_at(app.library.maps[app.selected_map].beats,
                   music_position(app.audio) - app.audio_offset);
  return beat_at({}, app.now * 1000);
}

}  // namespace

void menu_enter(App& app) {
  auto& menu = app.menu;
  if (menu.particles.empty())
    for (i32 i = 0; i < kParticles; ++i)
      menu.particles.push_back(
          spawn_particle(app, random_between(0, app.height)));
  if (app.selected_map >= 0)
    select_map(app, app.selected_map, true);
  else
    play_random_song(app);
}

void menu_update(App& app) {
  auto& menu = app.menu;
  if (music_at_end(app.audio))
    play_random_song(app);

  f32 bands[MainMenu::kBands];
  audio_spectrum(app.audio, bands, MainMenu::kBands);
  for (i32 b = 0; b < MainMenu::kBands; ++b)
    menu.bands[b] = std::max(bands[b], menu.bands[b] - app.dt * 1.8f);

  const Beat beat = current_beat(app);
  if (beat.number != menu.last_beat) {
    menu.last_beat = beat.number;
    menu.rings.push_back({app.now, beat.kiai});
  }
  std::erase_if(menu.rings, [&](const BeatRing& ring) {
    return app.now - ring.start > 1;
  });

  // Particles drift upward, surging on the beat during kiai sections.
  const f32 pulse = static_cast<f32>(std::pow(1 - beat.phase, 4));
  const f32 speed = 1 + (beat.kiai ? 2.5f : 0.6f) * pulse;
  for (auto& particle : menu.particles) {
    particle.y -= particle.speed * speed * app.dt;
    if (particle.y + particle.size < 0)
      particle = spawn_particle(app, app.height + particle.size);
  }

  const i32 hovered = item_at(app, app.cursor);
  if (hovered != menu.hovered && hovered >= 0)
    play_sound(app.audio, Sound::MenuHover, 0.5f);
  menu.hovered = hovered;
  for (i32 i = 0; i < 3; ++i)
    menu.hover[i] =
        approach(menu.hover[i], hovered == i ? 1.0f : 0.0f, 14, app.dt);
}

void menu_draw(App& app) {
  auto&        r = app.renderer;
  const auto&  menu = app.menu;
  const Layout l = layout(app);
  draw_backdrop(app, 0.55f, 1);

  for (const auto& p : menu.particles)
    draw_triangle(r, p.x, p.y - p.size * 0.58f, p.x - p.size * 0.5f,
                  p.y + p.size * 0.29f, p.x + p.size * 0.5f,
                  p.y + p.size * 0.29f, fade(kWhite, p.alpha));

  const Beat beat = current_beat(app);
  const f32  pulse = static_cast<f32>(std::pow(1 - beat.phase, 4));
  const f32  radius =
      l.logo_radius *
      (1 + 0.035f * pulse * (beat.kiai ? 1.6f : 1) + 0.04f * menu.hover[0]);
  const Vec2 c = l.logo;

  for (const auto& ring : menu.rings) {
    const f32 t = static_cast<f32>(app.now - ring.start);
    const f32 size = radius * (1 + 0.5f * ease_out(t));
    draw_circle(r, c.x, c.y, size,
                fade(kWhite, (ring.kiai ? 0.35f : 0.12f) * (1 - t)),
                1 - 3 / size);
  }

  // The spectrum, mirrored so bass sits at the top and bottom of the logo.
  for (i32 i = 0; i < kBars; ++i) {
    const i32  half = kBars / 2;
    const i32  band = (i < half ? i : kBars - 1 - i) * MainMenu::kBands / half;
    const f32  value = menu.bands[band];
    const f32  angle = i * kTau / kBars + static_cast<f32>(app.now) * 0.15f;
    const Vec2 d{std::cos(angle), std::sin(angle)};
    const f32  inner = radius * 1.03f;
    const f32  outer = inner + radius * (0.03f + 0.5f * value * value);
    draw_line(r, c.x + d.x * inner, c.y + d.y * inner, c.x + d.x * outer,
              c.y + d.y * outer, kTau * radius / kBars * 0.55f,
              fade(kWhite, 0.3f));
  }

  draw_circle(r, c.x, c.y + radius * 0.04f, radius * 1.07f, fade(kBlack, 0.45f),
              0, 0.25f);
  draw_circle(r, c.x, c.y, radius, kWhite);
  draw_circle_gradient(r, c.x, c.y, radius * 0.92f, hex_color(0xFF77B4),
                       hex_color(0xB23A8E));
  draw_circle(r, c.x, c.y, radius * 0.92f, fade(kWhite, 0.12f + 0.2f * pulse),
              0.94f);
  const f32 title_size = radius * 0.42f;
  draw_text_shadowed(r, app.heavy, "circles!", c.x,
                     centered_baseline(app.heavy, title_size, c.y), title_size,
                     kWhite, Align::Center);

  draw_button(app, l.button_x, l.button_y[0], l.button_w, l.button_h, "PLAY",
              menu.hover[1], hex_color(0xFF66AA));
  draw_button(app, l.button_x, l.button_y[1], l.button_w, l.button_h, "EXIT",
              menu.hover[2], hex_color(0x6655CC));

  if (app.selected_map >= 0) {
    const auto& info = app.library.maps[app.selected_map];
    draw_text_shadowed(r, app.regular, "NOW PLAYING", app.width - 20, 32, 13,
                       fade(kWhite, 0.7f), Align::Right);
    draw_text_shadowed(r, app.bold,
                       fit_text(app.bold, info.artist + " - " + info.title, 18,
                                app.width * 0.5f),
                       app.width - 20, 56, 18, kWhite, Align::Right);
  }
  const std::string footer =
      app.library.maps.empty()
          ? "No beatmaps yet: put .osz files or beatmap folders in beatmaps/ "
            "and restart"
          : std::to_string(app.library.maps.size()) + " beatmaps in " +
                std::to_string(app.library.sets.size()) + " sets";
  draw_text_shadowed(r, app.regular, footer, 20, app.height - 20, 15,
                     fade(kWhite, 0.8f));
  char offset[96];
  snprintf(offset, sizeof(offset),
           "Audio offset %+.0f ms   (- / =, Shift for bigger steps)",
           app.audio_offset);
  draw_text_shadowed(r, app.regular, offset, 20, app.height - 42, 13,
                     fade(kWhite, 0.6f));
}

void menu_key(App& app, i32 key, i32 action) {
  if (action != GLFW_PRESS)
    return;
  if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER || key == GLFW_KEY_P)
    activate(app, 1);
  else if (key == GLFW_KEY_ESCAPE)
    activate(app, 2);
}

void menu_click(App& app, i32 button, i32 action) {
  if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS)
    activate(app, item_at(app, app.cursor));
}

}  // namespace circles
