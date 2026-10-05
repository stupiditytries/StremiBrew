// The app's screens. One object owns the focus, the animations and the drawing; the host
// (the console's main loop or the PC preview) feeds it button presses and time and asks it
// to draw each frame.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "account_data.hpp"
#include "board_data.hpp"
#include "details_data.hpp"
#include "player_screen.hpp"
#include "speech_models.hpp"

struct NVGcontext;

namespace ui
{
class Images;
class DetailsScreen;
class PlayerScreen;

enum class Button
{
    Up,
    Down,
    Left,
    Right,
    Accept,
    Back,
    SkipBack,    // L1: in the player, back 15 seconds
    SkipForward, // R1: forward 15 seconds
    Calibrate,   // triangle held: in the player, time the subtitles to the dialogue
    Options,     // the options button: what can be done with the focused title
    Voice,       // square: search by saying what to search for
};

// Which set of rows the screen is showing, and so which the host should supply.
enum class View
{
    Board,   // the account's catalogs
    Search,  // what a search found
    Library, // the account's library
};

// What the options menu can ask the host to do with a title.
enum class LibraryAction
{
    Add,    // put it in the library
    Remove, // take it out
    Forget, // take it out of "Continue watching"
};

// How a voice search is going, for the search bar to say.
enum class VoicePhase
{
    Idle,
    Listening,
    Working,
    Failed,
};

// Sound effects the host plays as the user gets about.
enum class Sound
{
    Move,   // the focus moved
    Select, // something was chosen
    Back,   // stepped back out of something
};

// Things the user asks for that the host carries out.
enum class Intent
{
    SignIn,       // start signing in (ask for a link code)
    CancelSignIn, // abandon the sign-in in progress
    SignOut,
};

// Trailers on the home screen. The UI says when a title has had the focus long enough to
// be worth getting its trailer ready, when to play it, and when the focus has moved on.
struct TrailerHandler
{
    std::function<void(const std::string &id)> prepare;
    std::function<void()> start;
    std::function<void()> stop;
};

// What the host does for a title's page.
struct TitleHandler
{
    // Load a title's details; the answer comes back through App::set_details.
    std::function<void(const std::string &type, const std::string &id)> open;
    // Load the streams for one of the open title's videos (an episode).
    std::function<void(const std::string &type, const std::string &id, const std::string &video)>
        select_video;
    std::function<void()> close;
    // Play a stream of the open title; `title` is what the player shows. `type` and `id`
    // are the title's, and `video` is what is being played: an episode's id, or the
    // film's own.
    std::function<void(const Stream &stream, const std::string &title, const std::string &type,
                       const std::string &id, const std::string &video)>
        play;
};

class App
{
  public:
    // `font_folder` holds the Plus Jakarta Sans files; `image_folder` is the image cache.
    App(NVGcontext *context, const std::string &font_folder, const std::string &image_folder);
    ~App();

    void set_board(std::vector<BoardRow> rows);
    // The rows wanted now, and for a search the words searched for. The host supplies
    // rows for these through set_board (and no others).
    View view() const;
    const std::string &search_query() const
    {
        return search_sent_;
    }
    // Replaces the words being searched for (a voice search's result).
    void set_search_query(const std::string &text);
    void set_library_handler(std::function<void(LibraryAction, const BoardItem &)> handler);
    // Voice search: what starts one (not set when there is no way to hear), and how the
    // one in progress is going.
    void set_voice_handler(std::function<void()> start);
    void set_voice(VoicePhase phase, const std::string &message);
    // How images that are not in the cache folder yet are downloaded (see Images).
    void set_image_fetcher(
        std::function<void(const std::string &address, const std::string &file)> fetch,
        std::function<bool(const std::string &address)> failed);
    void set_account(Account account);
    const Account &account() const
    {
        return account_;
    }
    void set_title_handler(TitleHandler handler);
    // The open title's details, as they arrive and change.
    void set_details(Details details);
    bool title_open() const
    {
        return title_open_ || veil_rising_;
    }
    void set_intent_handler(std::function<void(Intent)> handler);
    void set_sound_handler(std::function<void(Sound)> handler);
    // The video player. It opens when a stream is chosen (TitleHandler::play starts the
    // host's player); while it is open the host draws the picture and the app draws only
    // the controls over it, from what the host reports with set_playback.
    void set_player_handler(PlayerHandler handler);
    void set_playback(const Playback &playback);
    // How far through being held the calibrate button (triangle) is, 0 to 1; the host
    // sends Button::Calibrate when it gets there.
    void set_calibrate_hold(float progress);
    void set_player_tracks(PlayerTracks tracks);
    // Called when the preferred audio or subtitle language is changed in Settings
    // (three-letter codes; an empty subtitle language is "off").
    // How subtitles look: set at start-up from what the host saved, and handed back to
    // the host to save when it is changed in Settings.
    void set_subtitle_style(const SubtitleStyle &style);
    const SubtitleStyle &subtitle_style() const
    {
        return subtitle_style_;
    }
    void set_subtitle_style_handler(std::function<void(const SubtitleStyle &)> handler);
    // The speech models for auto-calibrate: what the host has, and what Settings asks of
    // it (use this model; download this model).
    void set_trailer_handler(TrailerHandler handler);
    // The image the host draws trailers into (0 until it has one), whether a trailer is
    // playing in it now, and how much of the image's height is black bars at its top and
    // bottom (the featured area zooms in past them).
    void set_trailer(int image, bool live, float bar_top, float bar_bottom);
    // Whether trailers play on the home screen (a Settings switch; the host saves it).
    bool trailer_previews() const
    {
        return trailers_;
    }
    void set_trailer_previews(bool on)
    {
        trailers_ = on;
    }
    void set_speech_models(const SpeechModels &models);
    void set_speech_handlers(std::function<void(int model)> choose,
                             std::function<void(int model)> download);
    void set_languages_handler(
        std::function<void(const std::string &audio, const std::string &subtitles)> handler);
    bool player_open() const
    {
        return player_open_;
    }
    // The catalog (by its position among all of the board's catalogs) of the row the
    // focus is on, so the host can load rows ahead of it.
    std::size_t focused_catalog() const
    {
        return row_focus_ < rows_.size() ? rows_[row_focus_].index : 0;
    }
    void press(Button button);
    void update(float seconds);
    // Draws one frame into a framebuffer of the given size in pixels.
    void draw(int width, int height);

  private:
    enum class Zone
    {
        Navigation,
        Search,
        Rows,
        Content, // the one button of a screen that is not the board
    };

    void draw_navigation();
    void draw_top_bar();
    void draw_rows();
    void draw_hero();
    const BoardItem *focused_item() const;
    void draw_row(const BoardRow &row, std::size_t index, float top);
    float row_height(const BoardRow &row) const;
    float card_width(const BoardRow &row) const;
    float poster_height(const BoardRow &row) const;
    float row_top(std::size_t index) const;
    void follow_focus();
    void enter_tab();
    void apply(Button button);
    bool press_keyboard(Button button);
    bool press_menu(Button button);
    void clear_search();
    void draw_keyboard();
    void draw_menu();
    void close_player();
    void change_language(bool subtitles, int step);
    void change_subtitle_style(int row, int step);
    // A number that changes whenever a press moves the focus or changes the screen.
    std::size_t focus_mark() const;
    void draw_settings();
    void draw_unbuilt_tab();

    NVGcontext *vg_;
    std::unique_ptr<Images> images_;
    std::vector<BoardRow> rows_;
    Account account_;
    TitleHandler title_handler_;
    std::unique_ptr<DetailsScreen> details_;
    bool title_open_ = false;
    std::unique_ptr<PlayerScreen> player_;
    PlayerHandler player_handler_;
    bool player_open_ = false;
    float player_leaving_ = 0; // 1..0: the black the player leaves behind, fading away
    // Going between the board and a title's page fades through black, so that the two
    // are never both on screen: 0 is the board, 1 is black.
    float veil_ = 0;
    bool veil_rising_ = false; // heading for a title's page
    std::string pending_type_, pending_id_, pending_name_, pending_video_;
    Details pending_known_;
    std::string title_type_, title_id_;
    std::function<void(Intent)> intent_;
    std::function<void(Sound)> sound_;
    std::function<void(LibraryAction, const BoardItem &)> library_handler_;
    std::function<void()> voice_handler_;
    VoicePhase voice_phase_ = VoicePhase::Idle;
    std::string voice_message_;
    float voice_shown_ = 0; // seconds a failure has been on show
    // Search: the words as typed so far, and as last sent to be searched for (typing is
    // followed at a short remove, so every letter is not a search of its own).
    std::string search_text_, search_sent_;
    float search_wait_ = 0;
    // The on-screen keyboard under the search bar, and the key the focus is on.
    bool keyboard_open_ = false;
    int key_row_ = 1, key_column_ = 0;
    // The view the rows on screen belong to.
    View shown_view_ = View::Board;
    std::string shown_query_;
    // The options menu for a title: the title, what can be done with it, and the focus.
    bool menu_open_ = false;
    BoardItem menu_item_;
    std::vector<std::pair<std::string, LibraryAction>> menu_choices_;
    int menu_focus_ = 0;
    float menu_x_ = 0, menu_y_ = 0;
    std::function<void(const std::string &, const std::string &)> languages_;
    // Settings, in the order the focus goes through them: the account button (0), the
    // two language rows (1, 2), trailers (3), the four subtitle style rows (4 to 7), and
    // auto-calibrate's model, download and offset (8 to 10).
    int content_focus_ = 0;
    SpeechModels speech_;
    // Trailers: the switch; the title whose trailer is being got ready or played and how
    // long the focus has rested on it; and how visible the trailer is over the artwork.
    bool trailers_ = true;
    TrailerHandler trailer_handler_;
    std::string trailer_for_;
    float trailer_dwell_ = 0;
    bool trailer_prepared_ = false, trailer_started_ = false;
    int trailer_image_ = 0;
    bool trailer_live_ = false;
    float trailer_alpha_ = 0;
    float trailer_bar_top_ = 0, trailer_bar_bottom_ = 0; // as reported
    float trailer_trim_top_ = 0, trailer_trim_bottom_ = 0; // as drawn: they ease to those
    // Changing the speech model slides its name and its download status in from the side
    // the change came from: how far off they still are, and how visible.
    float speech_shift_ = 0, speech_alpha_ = 1;
    std::function<void(int)> choose_speech_, download_speech_;
    SubtitleStyle subtitle_style_;
    std::function<void(const SubtitleStyle &)> subtitle_style_handler_;

    Zone zone_ = Zone::Rows;
    int navigation_focus_ = 0; // which navigation button the focus is on
    int selected_tab_ = 0;     // which screen is shown
    std::size_t row_focus_ = 0;
    std::vector<std::size_t> column_focus_; // focused card in each row

    // Scroll positions ease towards their targets every frame.
    float scroll_y_ = 0, scroll_y_target_ = 0;
    std::vector<float> scroll_x_, scroll_x_target_;
    float focus_pulse_ = 0; // 0..1, eases to 1 after the focus moves
    // The featured area: the item it shows (a copy, so a board refresh cannot pull it
    // away), the item the focus is on, how long the focus has rested there, and how
    // visible its text, title logo and background artwork each are.
    BoardItem hero_;
    bool hero_valid_ = false;
    std::string hero_target_;
    float hero_dwell_ = 0;
    float hero_alpha_ = 0;
    float hero_logo_alpha_ = 0;
    float hero_art_alpha_ = 0;
    float hero_logo_wait_ = 0; // seconds spent waiting for the shown item's logo
    bool hero_named_ = false;  // the shown item's name was written out instead of its logo
    float frame_seconds_ = 0; // the last update's time step, for fades advanced while drawing
    // How visible each navigation button's label and highlight are, 0..1. A tab's name
    // shows only while the focus is on it, and fades in and out as the focus moves.
    static constexpr int kMaxTabs = 8;
    float navigation_reveal_[kMaxTabs] = {};
};
} // namespace ui
