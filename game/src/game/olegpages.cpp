#include "game/olegpages.h"

#include "game/clock.h"
#include "game/colour.h"
#include "game/controllers.h"
#include "game/gamecontroller.h"
#include "game/language.h"
#include "game/math.h"
#include "game/memory.h"
#include "game/oleg.h"
#include "game/pads.h"
#include "game/renderer.h"
#include "game/savemanager.h"
#include "game/sound.h"
#include "game/widgets.h"
#include "platform/graphics.h"
#include "platform/system.h"
#include "retail/libc.h"

extern "C"
{
    // The header's constants the start-up sets for this file, which nothing reads: up (0, 1, 0, 1), the UI's shadow (its offset
    // and colour) and 45 degrees (65536ths); and its own: (0, 1, -1, 1), (0, 1, 0, 1), 0, white
    // and black
    extern Vector4 g_OlegPagesUnusedUp RETAIL(D_0030BDE0);
    extern f32 g_OlegPagesUnusedShadowX RETAIL(D_0030A5D8);
    extern f32 g_OlegPagesUnusedShadowY RETAIL(D_0030A5DC);
    extern u32 g_OlegPagesUnusedShadowColour RETAIL(D_0030A5E0);
    extern s32 g_OlegPagesUnusedAngle RETAIL(D_0030A5E8);
    extern Vector4 g_OlegPagesUnusedUpBack RETAIL(D_0030BDF0);
    extern Vector4 g_OlegPagesUnusedUp2 RETAIL(D_0030BE00);
    extern u32 g_OlegPagesUnusedZero RETAIL(D_0030A5F0);
    extern u32 g_OlegPagesUnusedWhite RETAIL(D_0030A5F8);
    extern u32 g_OlegPagesUnusedBlack RETAIL(D_0030A5FC);
}

namespace
{
// Memory for an object of a type
template <typename T> T* Allocate()
{
    return static_cast<T*>(MemoryAllocate(sizeof(T)));
}

// The main menu's title and its items' texts
constexpr const char* MainMenuTitle = "crash twinsanity";
constexpr u32 NewGameText = 9;
constexpr u32 LoadGameText = 0xA;
constexpr u32 OptionsText = 0xB;
// Its items' IDs
constexpr u32 NewGameItem = 0;
constexpr u32 LoadGameItem = 2;
constexpr u32 OptionsItem = 0x1E;
// A screen of OLEG's is shown over half a second, the others hidden; an item's gallery or movie starts half a second after it's
// picked
constexpr f32 ScreenChangeSeconds = 0.5f;
constexpr f32 ItemDelaySeconds = 0.5f;
// "continue", the ID of the items going back to a page's parent, and how far a frame moves the screen
constexpr u32 ContinueText = 5;
constexpr u32 ParentBackItem = 0x1D;
constexpr f32 ScreenStepX = Rounded(0.015);
constexpr f32 ScreenStepY = Rounded(0.03);

// The game over page: its title, "continue" (restarting from the save) and "quit game"
constexpr u32 GameOverTitle = 0x44;
constexpr u32 RestartItemId = 9;
constexpr u32 QuitGameText = 0x1A;
constexpr u32 QuitGameItem = 0xB;
// The confirmations: "yes" and "no" (its ID 0x1C), the quit's yes (its ID 0xB) and the disable autosave's (0xA)
constexpr u32 YesText = 3;
constexpr u32 NoText = 2;
constexpr u32 NoItem = 0x1C;
constexpr u32 QuitYesItem = 0xB;
constexpr u32 DisableAutosaveYesItem = 0xA;
// The notices' "continue" (its ID 0xC)
constexpr u32 NoticeContinueItem = 0xC;
// The options: the pages' titles, their links (IDs 6, 7, 8) and back (0x1C on the sub-pages)
constexpr u32 GraphicOptionsText = 0xD;
constexpr u32 SoundOptionsText = 0xE;
constexpr u32 GameOptionsText = 0xF;
constexpr u32 GraphicOptionsItem = 6;
constexpr u32 SoundOptionsItem = 7;
constexpr u32 GameOptionsItem = 8;
constexpr u32 BackText = 4;
constexpr u32 SubPageBackItem = 0x1C;
// Their items: vibration (no and yes), centring the screen, widescreen (off and on), the volumes (0 to 10), the output type and
// its choices (mono, stereo, Dolby Pro Logic II)
constexpr u32 VibrationText = 0x15;
constexpr u32 VibrationItem = 0x1A;
// TwinsanityEvolved (D49, D56): the game options' two language rows -- TEXT LANGUAGE (the text table's "language", 0xC,
// which retail never showed: the console's language stood; titled by the platform when it has a word for it) and SPEECH
// LANGUAGE (titled by the platform), their choices the TEXT languages and the SPEECH languages the inserted disc carries (the
// platform's enumeration: Platform::System::TextLanguage* / SpeechLanguage*), IDs no retail item of the page has. Each row's
// value shown (what Entered set, what Frame last applied): a change of it is the player's step
constexpr u32 LanguageText = 0xC;
constexpr u32 LanguageItem = 0x1F;
constexpr u32 SpeechLanguageItem = 0x1B;
constexpr u32 LanguageLabelSize = 32;
constexpr u32 LanguagesMax = 16;
constexpr const char* LanguageVoicesFolder = "Crash6\\";
constexpr const char* SpeechLanguageTitle = "speech language";
s32 g_LanguageRowShown = -1;
s32 g_SpeechLanguageRowShown = -1;
// The speech row and the rows' titles are STATIC (the module's own data: D38): the heap holds exactly what the one language
// row held -- one choice item and every language's word once, which the speech row's choices point at
ChoiceItem g_SpeechLanguageRow;
const char* g_SpeechLanguageWords[LanguagesMax];
char g_LanguageRowTitles[2][LanguageLabelSize];
bool g_SpeechLanguageRowMade = false;
// The words asked of the platform while the rows are built (static, not on the stack: nothing the game reads later sees them)
char g_LanguageWords[LanguagesMax * 2][LanguageLabelSize];
char g_LanguageWord[LanguageLabelSize];
u32 g_SpeechLanguageWordOf[LanguagesMax];
// TwinsanityEvolved: the graphic options' platform rows -- the platform's own display settings (aspect, scale, filter,
// vsync, display mode on the PC), one ChoiceItem each with the platform's labels, IDs from 0x20 (none of the page's)
constexpr u32 DisplaySettingFirstItem = 0x20;
constexpr u32 DisplaySettingLabelSize = 32;
constexpr u32 DisplaySettingsMax = 8;
// TwinsanityEvolved: the Options menu's MODS page -- its link on the options page, one toggle row per mod (off / on) and,
// for a mod with a page of its own, a link row shown while the mod is on; a mod WITHOUT a switch (always available) has one row
// that opens its page at once and turns it on; a mod's page holds one action row per platform row. IDs none of the pages use
// (12 bits). Up to 16 mods (the toggles' and links' IDs and the actions' 4 bits of mod). The platform lists only the
// mods that apply while the game runs -- a mod of the mods folder changes the disc and is switched before the game starts
//
constexpr const char* ModsPageTitle = "mods";
constexpr u32 ModsPageItem = 0x30;
constexpr u32 ModToggleFirstItem = 0x40;
constexpr u32 ModLinkFirstItem = 0x50;
constexpr u32 ModRowFirstItem = 0x100;
constexpr u32 ModsMax = 16;
constexpr u32 ModRowsMax = 32;
constexpr u32 ModLabelSize = 48;
// An ActionItem's action for a mod page's row: the base, the mod in bits 8..11, the row in bits 0..7
constexpr u32 ActionModRow = 0x1000;
// An ActionItem's action for a switchless mod's row on the Mods page: the base, the mod in bits 8..11 -- the mod goes on and its
// page opens
constexpr u32 ActionModOpen = 0x2000;
OlegPage* BuildModPage(u32 mod);
constexpr u32 CentreScreenText = 0x11;
constexpr u32 CentreScreenItem = 0x15;
constexpr u32 WidescreenText = 0x10;
constexpr u32 WidescreenItem = 0x16;
constexpr u32 OffText = 0;
constexpr u32 OnText = 1;
constexpr u32 EffectsVolumeText = 0x12;
constexpr u32 EffectsVolumeItem = 0x17;
constexpr u32 MusicVolumeText = 0x13;
constexpr u32 MusicVolumeItem = 0x18;
constexpr s32 MaxVolume = 10;
constexpr f32 VolumeStep = Rounded(0.1);
constexpr u32 OutputTypeText = 0x14;
constexpr u32 OutputTypeItem = 0x19;
constexpr u32 OutputTypes = 3;
constexpr u32 MonoText = 6;
// The pause menu: its title, its pages' (save game, load game), its items' texts and IDs
constexpr u32 PauseTitle = 0x18;
constexpr u32 SaveGameText = 0x19;
constexpr u32 SaveGameItem = 1;
constexpr u32 DisableAutosaveText = 0x16;
constexpr u32 DisableAutosaveItem = 0xA;
constexpr u32 ResumeText = 0x1B;
constexpr u32 ResumeItem = 0xC;
// The extras: their title, the gem extras' titles (the gem's after the first), links (IDs 0xE on) and items (16 each, their IDs
// their indexes), the complete game's movie item and its texts before and once the game's done, and resume's ID
constexpr u32 ExtrasTitle = 0x17;
constexpr u32 GemExtrasTitle = 0x59;
constexpr u32 GemExtrasLink = 0xE;
constexpr u32 GemExtrasItems = 16;
constexpr u32 CompleteItem = 0x14;
constexpr u32 CompleteText = 0x61;
constexpr u32 NotCompleteText = 0x58;
constexpr u32 ExtrasResumeItem = 0x1C;
// The galleries' first texts and their pictures' names
constexpr u32 BossesText = 0x62;
constexpr u32 ConceptText = 0x72;
constexpr u32 EnemiesText = 0x82;
constexpr u32 MoviesText = 0x92;
constexpr u32 UnseenText = 0xA2;
constexpr u32 StoryboardsText = 0x47;
constexpr const char* BossesName = "Extras\\Bosses\\Boss";
constexpr const char* ConceptName = "Extras\\Concept\\Concept";
constexpr const char* EnemiesName = "Extras\\Enemies\\Enemy";
constexpr const char* UnseenName = "Extras\\Unseen\\Unseen";
// The movies' movies: the bonus movies and the story's
constexpr u32 ExtrasMovies[GemExtrasItems] = {
    GameMovie::FirstBonus, GameMovie::FirstBonus + 1, GameMovie::FirstBonus + 2, GameMovie::FirstBonus + 3,
    GameMovie::Intro,      GameMovie::Intro + 1,      GameMovie::Intro + 2,      GameMovie::Intro + 3,
    GameMovie::Intro + 4,  GameMovie::Intro + 5,      GameMovie::Intro + 6,      GameMovie::Intro + 7,
    GameMovie::Intro + 8,  GameMovie::Intro + 9,      GameMovie::Intro + 10,     GameMovie::Intro + 11,
};
// The movies needing the story done: from the 14th, once the story's area is 24
constexpr u32 FirstStoryEndMovie = 13;
constexpr u32 StoryDoneArea = 24;
// The levels' storyboards: their pictures' names and how many each has
struct Storyboard
{
    const char* name;
    u32 pictures;
};
constexpr Storyboard Storyboards[GemExtrasItems] = {
    {"Extras\\Storyboards\\02-Jungle\\Jungle", 8},           {"Extras\\Storyboards\\01-NSanity\\NSanity", 11},
    {"Extras\\Storyboards\\03-Cavern\\Cavern", 14},          {"Extras\\Storyboards\\04-Totem\\Totem", 15},
    {"Extras\\Storyboards\\06-IceClimb\\IceClimb", 17},      {"Extras\\Storyboards\\05-IceHub\\IceHub", 15},
    {"Extras\\Storyboards\\07-Slip\\Slip", 9},               {"Extras\\Storyboards\\08-HighSeas\\HighSeas", 14},
    {"Extras\\Storyboards\\09-Academy\\Academy", 8},         {"Extras\\Storyboards\\10-Boiler\\Boiler", 7},
    {"Extras\\Storyboards\\11-Classroom\\Classroom", 9},     {"Extras\\Storyboards\\12-Rooftop\\Rooftop", 7},
    {"Extras\\Storyboards\\13-Twinsanity\\Twinsanity", 5},   {"Extras\\Storyboards\\14-Rockslide\\Rockslide", 6},
    {"Extras\\Storyboards\\15-Pursuit\\Pursuit", 6},         {"Extras\\Storyboards\\16-AntAgony\\AntAgony", 10},
};
// The levels pages: each world's layout (the columns' places, the first row's height and the rows' spacing) and its four levels'
// areas (the items' IDs)
constexpr u32 LevelsPerWorld = 4;
struct LevelsLayout
{
    f32 evenX;
    f32 oddX;
    f32 top;
    f32 spacing;
    u32 levels[LevelsPerWorld];
};
constexpr u32 Worlds = 4;
constexpr LevelsLayout LevelsLayouts[Worlds] = {
    {Rounded(0.6), Rounded(0.4), Rounded(0.15), Rounded(0.15), {0, 1, 3, 4}},
    {Rounded(0.4), Rounded(0.6), Rounded(0.15), Rounded(0.15), {6, 7, 9, 10}},
    {Rounded(0.6), Rounded(0.4), Rounded(0.15), Rounded(0.15), {13, 15, 17, 18}},
    {Rounded(0.4), Rounded(0.6), Rounded(0.15), Rounded(0.15), {20, 21, 22, 23}},
};
// Past the last world (never asked for): the retail code's layout, with no levels (the retail code's are what its stack held)
constexpr LevelsLayout NoWorld = {Rounded(0.4), Rounded(0.6), Rounded(0.2), 0.125f, {}};
// The levels' and the save slots' rings (panels: AddPanelRings' discs) in the middle of their widgets, the level widgets' size
// and the levels' discs' radii
constexpr Vector2 LevelWidgetScale = {Rounded(0.6), Rounded(0.1)};
constexpr Vector2 LevelPanelRadii = {Rounded(0.3), Rounded(0.05)};
// The save slots page: a save code item per slot (IDs from 0x100), and a ring panel (0.2 by 0.075) behind each slot's widget, in
// two staggered columns (the even slots' sliding in from the left)
constexpr u32 SlotItemIds = 0x100;
constexpr f32 SlotEvenX = Rounded(0.3);
constexpr f32 SlotOddX = Rounded(0.7);
constexpr f32 SlotsTop = Rounded(0.26);
constexpr f32 SlotSpacing = Rounded(0.09);
constexpr Vector2 SlotPanelRadii = {Rounded(0.2), Rounded(0.075)};

// Every other screen hidden and the screen shown, over half a second
void ShowScreen(OLEG& oleg, u32 screen)
{
    oleg.Hide(oleg.screens[OLEG::ScreenAllButOverlays], static_cast<s32>(g_ClockUnitsPerSecond * ScreenChangeSeconds), 0);
    oleg.Show(oleg.screens[screen], static_cast<s32>(g_ClockUnitsPerSecond * ScreenChangeSeconds), 0);
}

// A gallery item of a text and ID: its pictures (first to last) and their name
GalleryItem* MakeGalleryItem(u32 text, u32 id, const char* name, u32 first, u32 last)
{
    auto* item = Allocate<GalleryItem>();
    LinkItem::Construct(item, text, id, nullptr, MenuPlayers);
    item->vtable = g_GalleryItemVTable;
    StringConstruct(&item->name, name);
    item->first = 0;
    item->last = 0;
    item->unused16[0] = 0;
    item->unused16[1] = 0;
    item->first = static_cast<u8>(first);
    item->last = static_cast<u8>(last);
    return item;
}

// A gem extras page made: titled by its gem, its vtable, back (made first, added last)
BackItem* StartGemExtras(GemExtrasPage* page, u32 gem, MenuPage* parent, const GccVTableEntry* vtable)
{
    OlegPage::ConstructTitled(page, GemExtrasTitle + gem, parent);
    page->gem = gem;
    page->vtable = vtable;
    return BackItem::Construct(Allocate<BackItem>(), BackText, ParentBackItem, parent, MenuPlayers);
}

// The galleries of a gem extras page: each of so many pictures, numbered on from the last
void AddGalleries(GemExtrasPage* page, u32 text, const char* name, u32 pictures)
{
    u32 first = 1;
    u32 last = pictures;
    for (u32 item = 0; item < GemExtrasItems; item++)
    {
        page->Add(MakeGalleryItem(text + item, item, name, first, last));
        first += pictures;
        last += pictures;
    }
}

// A confirmation's yes and its no (back to the parent, selected first)
void AddYesNo(OlegPage* page, u32 yesId, u32 action, MenuPage* parent)
{
    ActionItem* yes = ActionItem::Construct(Allocate<ActionItem>(), YesText, yesId, action);
    BackItem* no = BackItem::Construct(Allocate<BackItem>(), NoText, NoItem, parent, MenuPlayers);
    page->Add(yes);
    page->Add(no);
    page->SetFirstItem(no->Id());
}
}

OlegPage* OlegPage::Construct(OlegPage* page, const char* name, MenuPage* parent)
{
    MenuPage::Construct(page, name, MenuPlayers);
    page->parent = parent;
    page->vtable = g_OlegPageVTable;
    return page;
}

OlegPage* OlegPage::ConstructTitled(OlegPage* page, u32 title, MenuPage* parent)
{
    MenuPage::ConstructTitled(page, title, MenuPlayers);
    page->parent = parent;
    page->vtable = g_OlegPageVTable;
    return page;
}

void OlegPage::Destroy(u32 flags)
{
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

NewGamePage* NewGamePage::Construct(NewGamePage* page, MainMenuPage* owner)
{
    OlegPage::ConstructTitled(page, NewGameText, nullptr);
    page->owner = owner;
    page->vtable = g_NewGamePageVTable;
    return page;
}

void NewGamePage::Destroy(u32 flags)
{
    OlegPage::Destroy(flags);
}

void NewGamePage::Entered(u32, u32)
{
    g_OlegGameController->LoadForNewGame();
}

LoadGamePage* LoadGamePage::Construct(LoadGamePage* page, OlegPage* owner)
{
    OlegPage::ConstructTitled(page, LoadGameText, nullptr);
    page->owner = owner;
    page->vtable = g_LoadGamePageVTable;
    return page;
}

void LoadGamePage::Destroy(u32 flags)
{
    OlegPage::Destroy(flags);
}

void LoadGamePage::Entered(u32, u32)
{
    g_OlegGameController->LoadSavedGame();
}

ActionItem* ActionItem::Construct(ActionItem* item, u32 text, u32 id, u32 action)
{
    LinkItem::Construct(item, text, id, nullptr, MenuPlayers);
    item->action = action;
    item->vtable = g_ActionItemVTable;
    return item;
}

void ActionItem::Destroy(u32 flags)
{
    LinkItem::Destroy(flags);
}

MenuPage* ActionItem::Activate(u32 player, MenuPage* page)
{
    GameController* controller = g_OlegGameController;
    OLEG& oleg = controller->oleg;
    switch (action)
    {
    case ActionPauseMenu:
        controller->ReturnToPauseMenu();
        break;
    case ActionOptions:
        ShowScreen(oleg, OLEG::ScreenOptions);
        break;
    case ActionScreenPosition:
        ShowScreen(oleg, OLEG::ScreenScreenPosition);
        break;
    case ActionQuit:
        ShowScreen(oleg, OLEG::ScreenQuit);
        break;
    case ActionTitle:
        controller->QuitToTitle();
        break;
    case ActionDisableAutosave:
        ShowScreen(oleg, OLEG::ScreenDisableAutosave);
        break;
    case ActionStopSaving:
        controller->StopSaving();
        break;
    case ActionLoad:
        controller->LoadSavedGame();
        break;
    case ActionSave:
        controller->SaveFromPause();
        break;
    default:
        // TwinsanityEvolved: a switchless mod's row on the Mods page -- the mod goes on and its page (built now) opens
        if ((action & ActionModOpen) != 0)
        {
            u32 mod = (action >> 8) & 0xF;
            if (Platform::System::ModEnabled(mod) == 0)
            {
                Platform::System::SetModEnabled(mod, 1);
            }

            target = BuildModPage(mod);
        }
        // TwinsanityEvolved: a row of a mod's page -- the platform says what the game does
        else if ((action & ActionModRow) != 0)
        {
            u32 mod = (action >> 8) & 0xF;
            u32 row = action & 0xFF;
            u32 outcome = Platform::System::ModPageRowChosen(mod, row);
            if (outcome == Platform::System::ModActionWarp)
            {
                char chunk[128];
                u32 character = GameProgress::NoCharacter;
                Platform::System::ModWarpTarget(chunk, sizeof(chunk), &character);
                if (controller->WarpToChunk(chunk, character) != 0)
                {
                    oleg.Hide(oleg.screens[OLEG::ScreenAllButOverlays], static_cast<s32>(g_ClockUnitsPerSecond * ScreenChangeSeconds), 0);
                }
            }
            else if (outcome == Platform::System::ModActionTitle)
            {
                controller->QuitToTitle();
            }
            else if (outcome == Platform::System::ModActionCredits)
            {
                controller->PlayCredits();
            }
        }

        break;
    }

    return MenuItem::Activate(player, page);
}

MainMenuPage* MainMenuPage::Construct(MainMenuPage* page)
{
    OlegPage::Construct(page, MainMenuTitle, &g_ResumePage);
    page->vtable = g_MainMenuPageVTable;
    NewGamePage::Construct(&page->newGame, page);
    LoadGamePage::Construct(&page->loadGame, page);
    LinkItem* newGame = LinkItem::Construct(Allocate<LinkItem>(), NewGameText, NewGameItem,
                                            &page->newGame, MenuPlayers);
    LinkItem* loadGame = LinkItem::Construct(Allocate<LinkItem>(), LoadGameText, LoadGameItem,
                                             &page->loadGame, MenuPlayers);
    ActionItem* options = ActionItem::Construct(Allocate<ActionItem>(), OptionsText,
                                                OptionsItem, ActionItem::ActionOptions);
    page->Add(newGame);
    page->Add(loadGame);
    page->Add(options);
    return page;
}

void MainMenuPage::Destroy(u32 flags)
{
    loadGame.OlegPage::Destroy(DestroyOnly);
    newGame.OlegPage::Destroy(DestroyOnly);
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

void MainMenuPage::Frame(u32, MenuInput*, MenuSounds*)
{
}

ScreenPositionPage* ScreenPositionPage::Construct(ScreenPositionPage* page, MenuPage* parent)
{
    OlegPage::Construct(page, "", parent);
    page->vtable = g_ScreenPositionPageVTable;
    BackItem* item = BackItem::Construct(Allocate<BackItem>(), ContinueText, ParentBackItem,
                                         parent, MenuPlayers);
    page->Add(item);
    return page;
}

void ScreenPositionPage::Destroy(u32 flags)
{
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

void ScreenPositionPage::Frame(u32, MenuInput* input, MenuSounds*)
{
    GameRendererController* renderer = G_GameRendererController;
    Vector2 offset;
    CopyVector2(&offset, &renderer->screenOffset);
    if (input->Has(MenuInput::ActionLeft, 0) != 0)
    {
        offset.x -= ScreenStepX;
    }
    else if (input->Has(MenuInput::ActionRight, 0) != 0)
    {
        offset.x += ScreenStepX;
    }

    if (input->Has(MenuInput::ActionUp, 0) != 0)
    {
        offset.y -= ScreenStepY;
    }
    else if (input->Has(MenuInput::ActionDown, 0) != 0)
    {
        offset.y += ScreenStepY;
    }

    renderer->SetScreenOffset(&offset);
}

u32 ScreenPositionPage::Back(u32)
{
    GameController* controller = g_OlegGameController;
    OLEG& oleg = controller->oleg;
    oleg.Hide(oleg.screens[OLEG::ScreenAllButOverlays], static_cast<s32>(g_ClockUnitsPerSecond * ScreenChangeSeconds), 0);
    oleg.Show(oleg.screens[OLEG::ScreenOptions], static_cast<s32>(g_ClockUnitsPerSecond * ScreenChangeSeconds), 0);
    return 1;
}

void UnusedPage::Destroy(u32 flags)
{
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

void UnusedPage::Entered(u32, u32)
{
}

void UnusedPage::Left(u32, u32)
{
}

RestartItem* RestartItem::Construct(RestartItem* item, u32 text, u32 id, u32 entry)
{
    ChoiceItem::Construct(item, text, id, 0, 1, 0, MenuPlayers);
    item->vtable = g_RestartItemVTable;
    item->restart.value = 0;
    item->restart.restarts = 1;
    item->restart.entry = entry;
    return item;
}

void RestartItem::Destroy(u32 flags)
{
    ChoiceItem::Destroy(flags);
}

MenuPage* RestartItem::Activate(u32, MenuPage*)
{
    if (restart.restarts != 0)
    {
        g_OlegGameController->RequestRestart(restart.entry);
    }

    return nullptr;
}

GameOverPage* GameOverPage::Construct(GameOverPage* page)
{
    OlegPage::ConstructTitled(page, GameOverTitle, nullptr);
    page->vtable = g_GameOverPageVTable;
    RestartItem* restart = RestartItem::Construct(Allocate<RestartItem>(), ContinueText,
                                                  RestartItemId, EntrySaved);
    ActionItem* quit = ActionItem::Construct(Allocate<ActionItem>(), QuitGameText, QuitGameItem,
                                             ActionItem::ActionTitle);
    page->Add(restart);
    page->Add(quit);
    page->SetFirstItem(restart->Id());
    return page;
}

void GameOverPage::Destroy(u32 flags)
{
    OlegPage::Destroy(flags);
}

DisableAutosavePage* DisableAutosavePage::Construct(DisableAutosavePage* page, MenuPage* parent)
{
    OlegPage::ConstructTitled(page, DisableAutosaveTitleText, parent);
    page->vtable = g_DisableAutosavePageVTable;
    AddYesNo(page, DisableAutosaveYesItem, ActionItem::ActionStopSaving, parent);
    return page;
}

void DisableAutosavePage::Destroy(u32 flags)
{
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

u32 DisableAutosavePage::Back(u32)
{
    ShowScreen(g_OlegGameController->oleg, OLEG::ScreenPauseMenu);
    return 1;
}

QuitPage* QuitPage::Construct(QuitPage* page, MenuPage* parent)
{
    OlegPage::ConstructTitled(page, QuitTitleText, parent);
    page->vtable = g_QuitPageVTable;
    AddYesNo(page, QuitYesItem, ActionItem::ActionTitle, parent);
    return page;
}

void QuitPage::Destroy(u32 flags)
{
    OlegPage::Destroy(flags);
}

u32 QuitPage::Back(u32)
{
    ShowScreen(g_OlegGameController->oleg, OLEG::ScreenPauseMenu);
    return 1;
}

NoticePage* NoticePage::Construct(NoticePage* page, u32 toPauseMenu, MenuPage* parent)
{
    OlegPage::Construct(page, "", parent);
    page->vtable = g_NoticePageVTable;
    MenuItem* item;
    if (toPauseMenu != 0)
    {
        item = ActionItem::Construct(Allocate<ActionItem>(), ContinueText, NoticeContinueItem,
                                     ActionItem::ActionPauseMenu);
    }
    else
    {
        item = BackItem::Construct(Allocate<BackItem>(), ContinueText, NoticeContinueItem,
                                   &g_ResumePage, MenuPlayers);
    }

    page->Add(item);
    return page;
}

void NoticePage::Destroy(u32 flags)
{
    OlegPage::Destroy(flags);
}

void UnusedPage2::Destroy(u32 flags)
{
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

void UnusedPage2::Entered(u32, u32)
{
}

void UnusedPage2::Left(u32, u32)
{
}

void UnusedPage3::Destroy(u32 flags)
{
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

void UnusedPage3::Frame(u32, MenuInput*, MenuSounds*)
{
}

void GameOptionsPage::Destroy(u32 flags)
{
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

// TwinsanityEvolved: the language rows' titles asked again (a text language may bring its own words for them): the text
// row reads the game's own "language" while the platform has no title for it
static void RefreshLanguageRowTitles(MenuPage* page)
{
    MenuItem* text = page->Find(LanguageItem);
    if (text == nullptr)
    {
        return;
    }

    Platform::System::LanguageRowTitle(0, g_LanguageRowTitles[0], LanguageLabelSize);
    text->text = g_LanguageRowTitles[0][0] != '\0' ? g_LanguageRowTitles[0] : nullptr;
    if (g_SpeechLanguageRowMade)
    {
        Platform::System::LanguageRowTitle(1, g_LanguageRowTitles[1], LanguageLabelSize);
        g_SpeechLanguageRow.text = g_LanguageRowTitles[1][0] != '\0' ? g_LanguageRowTitles[1] : SpeechLanguageTitle;
    }
}

// A row's choice in force (NoLanguage, or past the row's choices: the first is shown, applied only when stepped)
static s32 LanguageShown(u32 selected, u32 choices)
{
    return selected < choices ? static_cast<s32>(selected) : 0;
}

// TwinsanityEvolved (D49, D56): a language row stepped -- the game takes the retail index (the speech's) and the text
// slot from the platform; a text step re-reads its two text files (every text on screen, this menu's included, is looked up by
// index each frame), a speech step (or a text step that moved the retail index: leaving a raw slot) swaps the front end's
// speech bank; a level's own voices and the FMV sound track follow g_CurrentLanguage at their next load
static void ApplyLanguages(bool texts)
{
    u32 index = static_cast<u32>(GameLanguageOf(Platform::System::Language()));
    bool voiceMoved = index != g_CurrentLanguage;
    u32 textLanguage = Platform::System::LanguageTextBank();
    SetGameLanguages(index, textLanguage == Platform::System::NoLanguageBank ? index : textLanguage);
    if (texts)
    {
        g_OlegGameController->LoadTexts();
    }

    if (!texts || voiceMoved)
    {
        String voices;
        StringConstruct(&voices, LanguageVoicesFolder);
        StringAppend(&voices, g_LanguageNames[g_CurrentLanguage]);
        SetVoiceBank(voices.string);
        StringDestroy(&voices);
    }
}

void GameOptionsPage::Entered(u32 player, u32)
{
    auto* pads = static_cast<GamePadController*>(G_GamePadController);
    Find(VibrationItem)->SetFlag(player, pads->flags.vibration != 0);
    if (MenuItem* language = Find(LanguageItem))
    {
        // the languages in force (a raw slot the run pinned may have none of a row's: its first choice is shown)
        g_LanguageRowShown = LanguageShown(Platform::System::TextLanguageSelected(), Platform::System::TextLanguageCount());
        language->SetIntValue(player, g_LanguageRowShown);
        if (g_SpeechLanguageRowMade)
        {
            g_SpeechLanguageRowShown = LanguageShown(Platform::System::SpeechLanguageSelected(), Platform::System::SpeechLanguageCount());
            g_SpeechLanguageRow.SetIntValue(player, g_SpeechLanguageRowShown);
        }

        RefreshLanguageRowTitles(this);
    }
}

void GameOptionsPage::Frame(u32 player, MenuInput*, MenuSounds*)
{
    auto* pads = static_cast<GamePadController*>(G_GamePadController);
    u8 vibration;
    Find(VibrationItem)->GetFlag(player, &vibration);
    if (vibration != 0)
    {
        pads->flags.vibration = 1;
    }
    else
    {
        pads->DisableVibration();
    }

    // TwinsanityEvolved (D49, D56): the language rows apply as they are stepped -- the platform keeps the choice (its
    // settings file; Language() and LanguageTextBank() answer for it from now on)
    MenuItem* language = Find(LanguageItem);
    if (language == nullptr)
    {
        return;
    }

    s32 row;
    language->GetIntValue(player, &row);
    if (row >= 0 && row != g_LanguageRowShown && static_cast<u32>(row) < Platform::System::TextLanguageCount())
    {
        g_LanguageRowShown = row;
        Platform::System::SetTextLanguage(static_cast<u32>(row));
        ApplyLanguages(true);
        RefreshLanguageRowTitles(this);
    }

    if (!g_SpeechLanguageRowMade)
    {
        return;
    }

    g_SpeechLanguageRow.GetIntValue(player, &row);
    if (row >= 0 && row != g_SpeechLanguageRowShown && static_cast<u32>(row) < Platform::System::SpeechLanguageCount())
    {
        g_SpeechLanguageRowShown = row;
        Platform::System::SetSpeechLanguage(static_cast<u32>(row));
        ApplyLanguages(false);
    }
}

void GameOptionsPage::Left(u32, u32)
{
}

void GraphicsOptionsPage::Destroy(u32 flags)
{
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

// TwinsanityEvolved: the platform rows re-read -- every row's value from the platform, and its choice names into the
// buffers the page allocated when it was built (a name may depend on another row: the vsync row reads "off (uncapped)"
// while the frame rate is uncapped; a value the platform refused snaps back)
static void SyncDisplaySettingRows(MenuPage* page, u32 player)
{
    u32 settings = Platform::Graphics::DisplaySettingCount();
    for (u32 setting = 0; setting < settings && setting < DisplaySettingsMax; setting++)
    {
        MenuItem* row = page->Find(DisplaySettingFirstItem + setting);
        if (row == nullptr)
        {
            continue;
        }

        row->SetIntValue(player, static_cast<s32>(Platform::Graphics::DisplaySettingValue(setting)));
        auto* choice = static_cast<ChoiceItem*>(row);
        if (choice->usesStrings != 0 && choice->strings != nullptr)
        {
            for (u32 c = 0; c < choice->choices; c++)
            {
                Platform::Graphics::DisplaySettingChoiceLabel(setting, c, const_cast<char*>(choice->strings[c]), DisplaySettingLabelSize);
            }
        }
    }
}

void GraphicsOptionsPage::Entered(u32 player, u32)
{
    Find(WidescreenItem)->SetFlag(player, g_WidescreenTv);
    // TwinsanityEvolved: the platform rows show the platform's choices
    SyncDisplaySettingRows(this, player);
}

void GraphicsOptionsPage::Frame(u32 player, MenuInput*, MenuSounds*)
{
    u8 widescreen;
    Find(WidescreenItem)->GetFlag(player, &widescreen);
    g_WidescreenTv = widescreen;
    // TwinsanityEvolved: a stepped platform row is applied at once (the platform keeps it)
    u32 settings = Platform::Graphics::DisplaySettingCount();
    for (u32 setting = 0; setting < settings && setting < DisplaySettingsMax; setting++)
    {
        MenuItem* row = Find(DisplaySettingFirstItem + setting);
        if (row == nullptr)
        {
            continue;
        }

        s32 choice;
        row->GetIntValue(player, &choice);
        if (choice >= 0 && static_cast<u32>(choice) != Platform::Graphics::DisplaySettingValue(setting))
        {
            Platform::Graphics::SetDisplaySetting(setting, static_cast<u32>(choice));
            // TwinsanityEvolved: the platform's answer back into every row (a refused value, a dependent row's names)
            SyncDisplaySettingRows(this, player);
        }
    }
}

SoundOptionsPage* SoundOptionsPage::Construct(SoundOptionsPage* page, MenuPage* parent)
{
    MenuPage::ConstructTitled(page, SoundOptionsText, MenuPlayers);
    page->parent = parent;
    page->vtable = g_SoundOptionsPageVTable;
    ValueItem* effects = ValueItem::Construct(Allocate<ValueItem>(), EffectsVolumeText, EffectsVolumeItem, 0, MaxVolume, 0, MaxVolume,
                                              MenuPlayers);
    ValueItem* music = ValueItem::Construct(Allocate<ValueItem>(), MusicVolumeText, MusicVolumeItem, 0, MaxVolume, 0, MaxVolume,
                                            MenuPlayers);
    ChoiceItem* output = ChoiceItem::Construct(Allocate<ChoiceItem>(), OutputTypeText, OutputTypeItem, OutputTypes, 1, 0, MenuPlayers);
    BackItem* back = BackItem::Construct(Allocate<BackItem>(), BackText, SubPageBackItem, parent, MenuPlayers);
    for (u32 choice = 0; choice < OutputTypes; choice++)
    {
        output->SetChoiceText(choice, static_cast<s32>(MonoText + choice));
    }

    page->Add(effects);
    page->Add(music);
    page->Add(output);
    page->Add(back);
    return page;
}

void SoundOptionsPage::Destroy(u32 flags)
{
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

void SoundOptionsPage::Entered(u32 player, u32)
{
    MenuItem* effects = Find(EffectsVolumeItem);
    MenuItem* music = Find(MusicVolumeItem);
    MenuItem* output = Find(OutputTypeItem);
    f32 effectsLevel = GroupVolumeLevel(EffectsGroup);
    f32 musicLevel = GroupVolumeLevel(MusicGroup);
    u32 stereo = g_MusicStereo;
    effects->SetIntValue(player, static_cast<s32>(effectsLevel * MaxVolume + 0.5f));
    music->SetIntValue(player, static_cast<s32>(musicLevel * MaxVolume + 0.5f));
    output->SetIntValue(player, static_cast<s32>(stereo));
}

void SoundOptionsPage::Frame(u32 player, MenuInput*, MenuSounds*)
{
    MenuItem* effects = Find(EffectsVolumeItem);
    MenuItem* music = Find(MusicVolumeItem);
    s32 value;
    if (Item(selections[player])->Id() == MusicVolumeItem)
    {
        music->GetIntValue(player, &value);
    }
    else
    {
        effects->GetIntValue(player, &value);
    }

    f32 level = static_cast<f32>(value) * VolumeStep;
    SetGroupVolume(level, level, EffectsGroup);
    SetGroupVolume(level, level, SecondEffectsGroup);
    SetGroupVolume(level, level, MovieGroup);
}

void SoundOptionsPage::Left(u32 player, u32)
{
    MenuItem* effects = Find(EffectsVolumeItem);
    MenuItem* music = Find(MusicVolumeItem);
    MenuItem* output = Find(OutputTypeItem);
    s32 values[3];
    effects->GetIntValue(player, &values[0]);
    music->GetIntValue(player, &values[1]);
    output->GetIntValue(player, &values[2]);
    f32 effectsLevel = static_cast<f32>(values[0]) * VolumeStep;
    f32 musicLevel = static_cast<f32>(values[1]) * VolumeStep;
    SetGroupVolume(effectsLevel, effectsLevel, EffectsGroup);
    SetGroupVolume(effectsLevel, effectsLevel, SecondEffectsGroup);
    SetGroupVolume(effectsLevel, effectsLevel, MovieGroup);
    SetGroupVolume(musicLevel, musicLevel, MusicGroup);
    SetMusicStereo(static_cast<u32>(values[2]));
}

namespace
{
// TwinsanityEvolved: the mods page and the mods' own pages. Their storage is STATIC (the module's own data, linked below
// the retail image: D38 -- the game's heap layout must not move for a switch nobody turned on; every pad log replays on its
// addresses), so with every mod off the heap sees only what MenuPage::Construct / Add / ConstructChoices take (a few words).
// A mod's own page (its rows) is built only once the mod is ON (at construction, or when its toggle is stepped)
OlegPage g_ModsPageStorage;
ToggleItem g_ModToggles[ModsMax];
LinkItem g_ModLinks[ModsMax];
ActionItem g_ModOpenRows[ModsMax];
BackItem g_ModsBackStorage;
OlegPage g_ModPageStorage[ModsMax];
ActionItem g_ModRowStorage[ModsMax][ModRowsMax];
BackItem g_ModPageBackStorage[ModsMax];
char g_ModLabels[ModsMax][ModLabelSize];
char g_ModPageTitles[ModsMax][ModLabelSize];
char g_ModRowLabels[ModsMax][ModRowsMax][ModLabelSize];
OlegPage* g_ModsPage = nullptr;
bool g_ModPageBuilt[ModsMax] = {};

// A mod's own page, built once (its rows from the platform) and linked from the mods page; the page, or null without one
OlegPage* BuildModPage(u32 mod)
{
    if (mod >= ModsMax)
    {
        return nullptr;
    }

    if (g_ModPageBuilt[mod])
    {
        return &g_ModPageStorage[mod];
    }

    u32 rows = Platform::System::ModPageRows(mod);
    if (rows == 0)
    {
        return nullptr;
    }

    if (rows > ModRowsMax)
    {
        rows = ModRowsMax;
    }

    g_ModPageBuilt[mod] = true;
    OlegPage* own = OlegPage::Construct(&g_ModPageStorage[mod], g_ModPageTitles[mod], g_ModsPage);
    for (u32 row = 0; row < rows; row++)
    {
        Platform::System::ModPageRowLabel(mod, row, g_ModRowLabels[mod][row], ModLabelSize);
        ActionItem* action = ActionItem::Construct(&g_ModRowStorage[mod][row], 0, ModRowFirstItem + row,
                                                   ActionModRow | (mod << 8) | row);
        action->text = g_ModRowLabels[mod][row];
        own->Add(action);
    }

    own->Add(BackItem::Construct(&g_ModPageBackStorage[mod], BackText, SubPageBackItem, g_ModsPage, MenuPlayers));
    g_ModLinks[mod].target = own;
    return own;
}

// A mod's toggle stepped on the mods page: the platform keeps it; the mod's page link shows while it is on
void ModValueStepped(ValueItem* item, u32 player)
{
    if (g_ModsPage == nullptr || item == nullptr)
    {
        return;
    }

    u32 id = item->Id();
    if (id < ModToggleFirstItem || id >= ModToggleFirstItem + ModsMax)
    {
        return;
    }

    u32 mod = id - ModToggleFirstItem;
    s32 value = 0;
    item->GetIntValue(player, &value);
    u32 enabled = value != 0 ? 1 : 0;
    if (enabled != Platform::System::ModEnabled(mod))
    {
        Platform::System::SetModEnabled(mod, enabled);
    }

    if (enabled != 0)
    {
        BuildModPage(mod);
    }

    if (MenuItem* link = g_ModsPage->Find(ModLinkFirstItem + mod))
    {
        u8 shown = enabled != 0 && g_ModPageBuilt[mod] ? EveryPlayer : 0;
        link->enabled = shown;
        link->shown = shown;
    }
}

OlegPage* ConstructModsPage(MenuPage* options)
{
    u32 mods = Platform::System::ModCount();
    if (mods == 0)
    {
        return nullptr;
    }

    if (mods > ModsMax)
    {
        mods = ModsMax;
    }

    OlegPage* page = OlegPage::Construct(&g_ModsPageStorage, ModsPageTitle, options);
    g_ModsPage = page;
    for (u32 mod = 0; mod < mods; mod++)
    {
        Platform::System::ModLabel(mod, g_ModLabels[mod], ModLabelSize);
        Platform::System::ModPageLabel(mod, g_ModPageTitles[mod], ModLabelSize);
        u32 enabled = Platform::System::ModEnabled(mod);
        if (Platform::System::ModHasSwitch(mod) == 0 && Platform::System::ModPageRows(mod) != 0)
        {
            // no switch: the mod's one row opens its page at once (the page is built then, so a run that never opens it
            // leaves the heap as it was) and turns the mod on
            g_ModPageBuilt[mod] = false;
            ActionItem* open = ActionItem::Construct(&g_ModOpenRows[mod], 0, ModLinkFirstItem + mod, ActionModOpen | (mod << 8));
            open->text = g_ModLabels[mod];
            page->Add(open);
            continue;
        }

        // the toggle: off / on in the game's own texts, applied at once (g_MenuValueStepped)
        ToggleItem* toggle = ToggleItem::Construct(&g_ModToggles[mod], 0, ModToggleFirstItem + mod, OffText, OnText, 1,
                                                   static_cast<s32>(enabled), MenuPlayers);
        toggle->text = g_ModLabels[mod];
        page->Add(toggle);
        // the mod's page: linked from a row shown while the mod is on; built when it is on
        if (Platform::System::ModPageRows(mod) == 0)
        {
            continue;
        }

        g_ModPageBuilt[mod] = false;
        LinkItem* link = LinkItem::ConstructNamed(&g_ModLinks[mod], g_ModPageTitles[mod], ModLinkFirstItem + mod, nullptr, MenuPlayers);
        if (enabled != 0)
        {
            BuildModPage(mod);
        }

        u8 shown = enabled != 0 && g_ModPageBuilt[mod] ? EveryPlayer : 0;
        link->enabled = shown;
        link->shown = shown;
        page->Add(link);
    }

    page->Add(BackItem::Construct(&g_ModsBackStorage, BackText, SubPageBackItem, options, MenuPlayers));
    g_MenuValueStepped = ModValueStepped;
    return page;
}

void DestroyModsPage()
{
    if (g_ModsPage == nullptr)
    {
        return;
    }

    g_MenuValueStepped = nullptr;
    for (u32 mod = 0; mod < ModsMax; mod++)
    {
        if (g_ModPageBuilt[mod])
        {
            g_ModPageStorage[mod].vtable = g_OlegPageVTable;
            g_ModPageStorage[mod].MenuPage::Destroy(DestroyOnly);
            g_ModPageBuilt[mod] = false;
        }
    }

    g_ModsPage->vtable = g_OlegPageVTable;
    g_ModsPage->MenuPage::Destroy(DestroyOnly);
    g_ModsPage = nullptr;
}
}

OptionsPage* OptionsPage::Construct(OptionsPage* page, MenuPage* parent)
{
    MenuPage::ConstructTitled(page, OptionsText, MenuPlayers);
    page->parent = parent;
    page->vtable = g_OptionsPageVTable;

    GameOptionsPage& game = page->game;
    MenuPage::ConstructTitled(&game, GameOptionsText, MenuPlayers);
    game.parent = page;
    game.vtable = g_GameOptionsPageVTable;
    ToggleItem* vibration = ToggleItem::Construct(Allocate<ToggleItem>(), VibrationText, VibrationItem, NoText, YesText, 1, 0, MenuPlayers);
    BackItem* gameBack = BackItem::Construct(Allocate<BackItem>(), BackText, SubPageBackItem, page, MenuPlayers);
    game.Add(vibration);
    // TwinsanityEvolved (D49, D56): the language rows, when the disc carries more than one language (text or speech):
    // TEXT LANGUAGE and SPEECH LANGUAGE, their choices the platform's, labelled by what each bank CONTAINS. Every language's
    // word is made once, on the heap, as the one language row's were (text languages first, then the speech languages that
    // have no text: the same words, the same blocks in the same order -- the heap is laid out as before); the text row's choice
    // item is that row's (its table holds every word, its range the text languages), the speech row (static) points at the
    // words of its languages
    u32 texts = Platform::System::TextLanguageCount();
    u32 speeches = Platform::System::SpeechLanguageCount();
    if (texts > LanguagesMax)
    {
        texts = LanguagesMax;
    }

    if (speeches > LanguagesMax)
    {
        speeches = LanguagesMax;
    }

    char (&words)[LanguagesMax * 2][LanguageLabelSize] = g_LanguageWords;
    u32 (&speechWord)[LanguagesMax] = g_SpeechLanguageWordOf;
    u32 all = 0;
    for (u32 row = 0; row < texts; row++)
    {
        Platform::System::TextLanguageLabel(row, words[all], LanguageLabelSize);
        all++;
    }

    for (u32 row = 0; row < speeches; row++)
    {
        char* word = g_LanguageWord;
        Platform::System::SpeechLanguageLabel(row, word, LanguageLabelSize);
        u32 at = 0;
        while (at < all && RetailLibc::StringCompare(words[at], word) != 0)
        {
            at++;
        }

        if (at == all)
        {
            RetailLibc::StringCopy(words[all], word);
            all++;
        }

        speechWord[row] = at;
    }

    if (all > g_LanguageCount * 2)
    {
        all = g_LanguageCount * 2;
    }

    g_SpeechLanguageRowMade = false;
    if (all > 1 && texts > 0)
    {
        u32 selected = Platform::System::TextLanguageSelected();
        ChoiceItem* language = ChoiceItem::Construct(Allocate<ChoiceItem>(), LanguageText, LanguageItem, all, 1,
                                                     LanguageShown(selected, texts), MenuPlayers);
        auto** labels = static_cast<const char**>(MemoryAllocate2(all * sizeof(const char*)));
        for (u32 word = 0; word < all; word++)
        {
            auto* label = static_cast<char*>(MemoryAllocate2(LanguageLabelSize));
            RetailLibc::StringCopy(label, words[word]);
            labels[word] = label;
        }

        language->strings = labels;
        language->usesStrings = 1;
        language->SetRange(0, static_cast<s32>(texts < all ? texts : all) - 1, 1, LanguageShown(selected, texts));
        game.Add(language);
        u32 choices = 0;
        for (u32 row = 0; row < speeches; row++)
        {
            if (speechWord[row] < all)
            {
                g_SpeechLanguageWords[choices] = labels[speechWord[row]];
                choices++;
            }
        }

        if (choices > 0)
        {
            // no choice table of its own (the words are the text row's): its choices set directly
            ChoiceItem::Construct(&g_SpeechLanguageRow, 0, SpeechLanguageItem, 0, 1, 0, MenuPlayers);
            g_SpeechLanguageRow.strings = g_SpeechLanguageWords;
            g_SpeechLanguageRow.usesStrings = 1;
            g_SpeechLanguageRow.choices = choices;
            g_SpeechLanguageRow.SetRange(0, static_cast<s32>(choices) - 1, 1,
                                         LanguageShown(Platform::System::SpeechLanguageSelected(), choices));
            g_SpeechLanguageRow.text = SpeechLanguageTitle;
            game.Add(&g_SpeechLanguageRow);
            g_SpeechLanguageRowMade = true;
        }

        RefreshLanguageRowTitles(&game);
    }

    game.Add(gameBack);

    GraphicsOptionsPage& graphics = page->graphics;
    MenuPage::ConstructTitled(&graphics, GraphicOptionsText, MenuPlayers);
    graphics.parent = page;
    graphics.vtable = g_GraphicsOptionsPageVTable;
    ActionItem* centre = ActionItem::Construct(Allocate<ActionItem>(), CentreScreenText, CentreScreenItem,
                                               ActionItem::ActionScreenPosition);
    ToggleItem* widescreen = ToggleItem::Construct(Allocate<ToggleItem>(), WidescreenText, WidescreenItem, OffText, OnText, 1, 0,
                                                   MenuPlayers);
    BackItem* graphicsBack = BackItem::Construct(Allocate<BackItem>(), BackText, SubPageBackItem, page, MenuPlayers);
    graphics.Add(centre);
    graphics.Add(widescreen);
    // TwinsanityEvolved: the platform's display settings as rows, labelled by the platform
    u32 settings = Platform::Graphics::DisplaySettingCount();
    for (u32 setting = 0; setting < settings && setting < DisplaySettingsMax; setting++)
    {
        u32 choices = Platform::Graphics::DisplaySettingChoices(setting);
        if (choices == 0)
        {
            continue;
        }

        ChoiceItem* row = ChoiceItem::Construct(Allocate<ChoiceItem>(), 0, DisplaySettingFirstItem + setting, choices, 1,
                                                static_cast<s32>(Platform::Graphics::DisplaySettingValue(setting)), MenuPlayers);
        auto* label = static_cast<char*>(MemoryAllocate2(DisplaySettingLabelSize));
        Platform::Graphics::DisplaySettingLabel(setting, label, DisplaySettingLabelSize);
        row->text = label;
        auto** labels = static_cast<const char**>(MemoryAllocate2(choices * sizeof(const char*)));
        for (u32 choice = 0; choice < choices; choice++)
        {
            auto* name = static_cast<char*>(MemoryAllocate2(DisplaySettingLabelSize));
            Platform::Graphics::DisplaySettingChoiceLabel(setting, choice, name, DisplaySettingLabelSize);
            labels[choice] = name;
        }

        row->strings = labels;
        row->usesStrings = 1;
        graphics.Add(row);
    }

    graphics.Add(graphicsBack);

    SoundOptionsPage::Construct(&page->sound, page);
    LinkItem* graphicsLink = LinkItem::Construct(Allocate<LinkItem>(), GraphicOptionsText, GraphicOptionsItem, &page->graphics,
                                                 MenuPlayers);
    LinkItem* soundLink = LinkItem::Construct(Allocate<LinkItem>(), SoundOptionsText, SoundOptionsItem, &page->sound, MenuPlayers);
    LinkItem* gameLink = LinkItem::Construct(Allocate<LinkItem>(), GameOptionsText, GameOptionsItem, &page->game, MenuPlayers);
    BackItem* back = BackItem::Construct(Allocate<BackItem>(), BackText, ParentBackItem, parent, MenuPlayers);
    page->Add(graphicsLink);
    page->Add(soundLink);
    page->Add(gameLink);
    // TwinsanityEvolved: the MODS page, when the platform ships mods
    if (OlegPage* mods = ConstructModsPage(page))
    {
        page->Add(LinkItem::ConstructNamed(Allocate<LinkItem>(), ModsPageTitle, ModsPageItem, mods, MenuPlayers));
    }

    page->Add(back);
    return page;
}

void OptionsPage::Destroy(u32 flags)
{
    DestroyModsPage();
    sound.vtable = g_OlegPageVTable;
    sound.MenuPage::Destroy(DestroyOnly);
    graphics.vtable = g_OlegPageVTable;
    graphics.MenuPage::Destroy(DestroyOnly);
    game.vtable = g_OlegPageVTable;
    game.MenuPage::Destroy(DestroyOnly);
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

u32 OptionsPage::Back(u32)
{
    GameController* controller = g_OlegGameController;
    OLEG& oleg = controller->oleg;
    oleg.Hide(oleg.screens[OLEG::ScreenAllButOverlays], static_cast<s32>(g_ClockUnitsPerSecond * ScreenChangeSeconds), 0);
    u32 screen =
        g_OlegGameController->State() == GameController::StateMainMenu ? OLEG::ScreenMainMenu : OLEG::ScreenPauseMenu;
    oleg.Show(oleg.screens[screen], static_cast<s32>(g_ClockUnitsPerSecond * ScreenChangeSeconds), 0);
    return 1;
}

void SaveGamePage::Destroy(u32 flags)
{
    OlegPage::Destroy(flags);
}

void SaveGamePage::Entered(u32, u32)
{
    g_OlegGameController->SaveFromPause();
}

PausePage* PausePage::Construct(PausePage* page, PadButtons*, Font*)
{
    OlegPage::ConstructTitled(page, PauseTitle, &g_ResumePage);
    page->vtable = g_PausePageVTable;
    SaveGamePage& saveGame = page->saveGame;
    OlegPage::ConstructTitled(&saveGame, SaveGameText, nullptr);
    saveGame.owner = page;
    saveGame.vtable = g_SaveGamePageVTable;
    LoadGamePage& loadGame = page->loadGame;
    OlegPage::ConstructTitled(&loadGame, LoadGameText, nullptr);
    loadGame.owner = page;
    loadGame.vtable = g_LoadGamePageVTable;
    QuitPage::Construct(&page->quit, page);
    ActionItem* options = ActionItem::Construct(Allocate<ActionItem>(), OptionsText, OptionsItem, ActionItem::ActionOptions);
    LinkItem* save = LinkItem::Construct(Allocate<LinkItem>(), SaveGameText, SaveGameItem, &page->saveGame, MenuPlayers);
    LinkItem* load = LinkItem::Construct(Allocate<LinkItem>(), LoadGameText, LoadGameItem, &page->loadGame, MenuPlayers);
    ActionItem* disableAutosave = ActionItem::Construct(Allocate<ActionItem>(), DisableAutosaveText, DisableAutosaveItem,
                                                        ActionItem::ActionDisableAutosave);
    ActionItem* quit = ActionItem::Construct(Allocate<ActionItem>(), QuitGameText, QuitGameItem, ActionItem::ActionQuit);
    BackItem* resume = BackItem::Construct(Allocate<BackItem>(), ResumeText, ResumeItem, &g_ResumePage, MenuPlayers);
    page->Add(options);
    page->Add(save);
    page->Add(load);
    page->Add(disableAutosave);
    page->Add(quit);
    page->Add(resume);
    page->SetFirstItem(ResumeItem);
    return page;
}

void PausePage::Destroy(u32 flags)
{
    quit.OlegPage::Destroy(DestroyOnly);
    loadGame.OlegPage::Destroy(DestroyOnly);
    saveGame.OlegPage::Destroy(DestroyOnly);
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

void PausePage::Frame(u32, MenuInput*, MenuSounds*)
{
    // The disable autosave item is there while an autosave waits or saves, not while its screens show
    MenuItem* disableAutosave = Find(DisableAutosaveItem);
    u32 saving = static_cast<u32>(g_OlegGameController->states.savingStep);
    bool autosaving = saving == GameController::SavingWait || saving == GameController::SavingSaved ||
                      saving == GameController::SavingSavedShown;
    bool shown = false;
    if (autosaving)
    {
        shown = g_OlegGameController->states.savingStep != GameController::SavingSavedShown;
    }

    disableAutosave->shown = shown ? EveryPlayer : 0;
}

void GalleryItem::Destroy(u32 flags)
{
    StringDestroy(&name);
    LinkItem::Destroy(flags);
}

MenuPage* GalleryItem::Activate(u32, MenuPage*)
{
    g_OlegGameController->RequestGallery(static_cast<s32>(g_ClockUnitsPerSecond * ItemDelaySeconds), name.string, first,
                                              last);
    return nullptr;
}

MovieItem* MovieItem::Construct(MovieItem* item, u32 text, u32 id, u32 movie)
{
    LinkItem::Construct(item, text, id, nullptr, MenuPlayers);
    item->movie = movie;
    item->vtable = g_MovieItemVTable;
    return item;
}

void MovieItem::Destroy(u32 flags)
{
    LinkItem::Destroy(flags);
}

MenuPage* MovieItem::Activate(u32, MenuPage*)
{
    g_OlegGameController->RequestMovie(static_cast<s32>(g_ClockUnitsPerSecond * ItemDelaySeconds), movie);
    return nullptr;
}

void GemExtrasPage::ShowFound()
{
    GameProgress& progress = g_OlegGameController->progress;
    for (u32 item = 0; item < GemExtrasItems; item++)
    {
        MenuItem* found = Find(item);
        found->enabled = (progress.levels[item].gems & 1u << gem) != 0 ? EveryPlayer : 0;
    }
}

BossesPage* BossesPage::Construct(BossesPage* page, u32 gem, MenuPage* parent)
{
    BackItem* back = StartGemExtras(page, gem, parent, g_BossesPageVTable);
    AddGalleries(page, BossesText, BossesName, 1);
    page->Add(back);
    return page;
}

void BossesPage::Destroy(u32 flags)
{
    OlegPage::Destroy(flags);
}

void BossesPage::Entered(u32, u32)
{
    ShowFound();
}

ConceptPage* ConceptPage::Construct(ConceptPage* page, u32 gem, MenuPage* parent)
{
    BackItem* back = StartGemExtras(page, gem, parent, g_ConceptPageVTable);
    AddGalleries(page, ConceptText, ConceptName, 2);
    page->Add(back);
    return page;
}

void ConceptPage::Destroy(u32 flags)
{
    OlegPage::Destroy(flags);
}

void ConceptPage::Entered(u32, u32)
{
    ShowFound();
}

EnemiesPage* EnemiesPage::Construct(EnemiesPage* page, u32 gem, MenuPage* parent)
{
    BackItem* back = StartGemExtras(page, gem, parent, g_EnemiesPageVTable);
    AddGalleries(page, EnemiesText, EnemiesName, 2);
    page->Add(back);
    return page;
}

void EnemiesPage::Destroy(u32 flags)
{
    OlegPage::Destroy(flags);
}

void EnemiesPage::Entered(u32, u32)
{
    ShowFound();
}

MoviesPage* MoviesPage::Construct(MoviesPage* page, u32 gem, MenuPage* parent)
{
    BackItem* back = StartGemExtras(page, gem, parent, g_MoviesPageVTable);
    for (u32 item = 0; item < GemExtrasItems; item++)
    {
        page->Add(MovieItem::Construct(Allocate<MovieItem>(), MoviesText + item, item, ExtrasMovies[item]));
    }

    page->Add(back);
    return page;
}

void MoviesPage::Destroy(u32 flags)
{
    OlegPage::Destroy(flags);
}

void MoviesPage::Entered(u32, u32)
{
    GameProgress& progress = g_OlegGameController->progress;
    for (u32 item = 0; item < GemExtrasItems; item++)
    {
        MenuItem* movie = Find(item);
        bool found = (progress.levels[item].gems & 1u << gem) != 0;
        if (item >= FirstStoryEndMovie)
        {
            found = found && progress.play.story >= StoryDoneArea;
        }

        movie->enabled = found ? EveryPlayer : 0;
    }
}

StoryboardsPage* StoryboardsPage::Construct(StoryboardsPage* page, u32 gem, MenuPage* parent)
{
    BackItem* back = StartGemExtras(page, gem, parent, g_StoryboardsPageVTable);
    for (u32 item = 0; item < GemExtrasItems; item++)
    {
        page->Add(MakeGalleryItem(StoryboardsText + item, item, Storyboards[item].name, 1, Storyboards[item].pictures));
    }

    page->Add(back);
    return page;
}

void StoryboardsPage::Destroy(u32 flags)
{
    OlegPage::Destroy(flags);
}

void StoryboardsPage::Entered(u32, u32)
{
    ShowFound();
}

UnseenPage* UnseenPage::Construct(UnseenPage* page, u32 gem, MenuPage* parent)
{
    BackItem* back = StartGemExtras(page, gem, parent, g_UnseenPageVTable);
    AddGalleries(page, UnseenText, UnseenName, 3);
    page->Add(back);
    return page;
}

void UnseenPage::Destroy(u32 flags)
{
    OlegPage::Destroy(flags);
}

void UnseenPage::Entered(u32, u32)
{
    ShowFound();
}

ExtrasPage* ExtrasPage::Construct(ExtrasPage* page)
{
    OlegPage::ConstructTitled(page, ExtrasTitle, &g_ResumePage);
    page->vtable = g_ExtrasPageVTable;
    BossesPage::Construct(&page->bosses, GemBlue, page);
    ConceptPage::Construct(&page->conceptArt, GemPurple, page);
    EnemiesPage::Construct(&page->enemies, GemGreen, page);
    MoviesPage::Construct(&page->movies, GemClear, page);
    StoryboardsPage::Construct(&page->storyboards, GemRed, page);
    UnseenPage::Construct(&page->unseen, GemYellow, page);
    MovieItem* complete = MovieItem::Construct(Allocate<MovieItem>(), CompleteText, CompleteItem, GameMovie::Complete);
    BackItem* resume = BackItem::Construct(Allocate<BackItem>(), ResumeText, ExtrasResumeItem, &g_ResumePage, MenuPlayers);
    // Every link made before any is added (adding can grow the page's list)
    OlegPage* const gemPages[GameProgress::Gems] = {&page->bosses,     &page->movies,      &page->enemies,
                                                    &page->conceptArt, &page->storyboards, &page->unseen};
    LinkItem* links[GameProgress::Gems];
    for (u32 gem = 0; gem < GameProgress::Gems; gem++)
    {
        links[gem] = LinkItem::Construct(Allocate<LinkItem>(), GemExtrasTitle + gem, GemExtrasLink + gem, gemPages[gem], MenuPlayers);
    }

    for (LinkItem* link : links)
    {
        page->Add(link);
    }

    page->Add(complete);
    page->Add(resume);
    return page;
}

void ExtrasPage::Destroy(u32 flags)
{
    unseen.OlegPage::Destroy(DestroyOnly);
    storyboards.OlegPage::Destroy(DestroyOnly);
    movies.OlegPage::Destroy(DestroyOnly);
    enemies.OlegPage::Destroy(DestroyOnly);
    conceptArt.OlegPage::Destroy(DestroyOnly);
    bosses.OlegPage::Destroy(DestroyOnly);
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

void ExtrasPage::Entered(u32, u32)
{
    GameProgress& progress = g_OlegGameController->progress;
    u32 done = progress.Done();
    MenuItem* complete = Find(CompleteItem);
    for (u32 gem = 0; gem < GameProgress::Gems; gem++)
    {
        MenuItem* link = Find(GemExtrasLink + gem);
        link->shown = progress.GemsFound(gem) != 0 ? EveryPlayer : 0;
    }

    if (done == 100)
    {
        complete->bits.text = CompleteText;
        complete->shown = EveryPlayer;
    }
    else
    {
        complete->bits.text = NotCompleteText;
        complete->shown = 0;
    }

    complete->text = nullptr;
}

LevelsPage* LevelsPage::Construct(LevelsPage* page, u32 world, MenuWidget* menu)
{
    OlegPage::Construct(page, "", &g_ResumePage);
    page->vtable = g_LevelsPageVTable;
    Widget* after = menu->next;
    const LevelsLayout& layout = world < Worlds ? LevelsLayouts[world] : NoWorld;
    MenuItem* items[Slots] = {};
    if (world < Worlds)
    {
        for (u32 level = 0; level < LevelsPerWorld; level++)
        {
            items[level] = LinkItem::ConstructNamed(Allocate<LinkItem>(), "", layout.levels[level], nullptr, MenuPlayers);
        }
    }

    Widget* tail = menu;
    for (u32 slot = 0; slot < Slots; slot++)
    {
        MenuItem* item = items[slot];
        if (item == nullptr)
        {
            page->rings[slot] = nullptr;
            page->levels[slot] = nullptr;
            continue;
        }

        Vector2 place = {(slot & 1) == 0 ? layout.evenX : layout.oddX, layout.top + layout.spacing * static_cast<f32>(slot)};
        Vector2 scale = {1.0f, 1.0f};
        Vector2 levelScale = LevelWidgetScale;
        Vector2 offset = {place.x < 0.5f ? -1.0f : 1.0f, 0.0f};
        RingWidget* ring =
            RingWidget::Construct(Allocate<RingWidget>(), AnchorMiddle, PanelRings, RingSegments, RingSteps, nullptr);
        page->rings[slot] = ring;
        LevelWidget* level = LevelWidget::Construct(Allocate<LevelWidget>(), AnchorMiddle, page, item, g_OlegGameController);
        page->levels[slot] = level;
        u32 colour;
        GetColor(&colour, ColourWhite);
        ring->SlideIn(colour, &place, &scale, &offset);
        GetColor(&colour, ColourWhite);
        level->SlideIn(colour, &place, &levelScale, &offset);
        Vector2 radii = LevelPanelRadii;
        AddPanelRings(&radii, ring);
        tail->next = ring;
        ring->next = level;
        tail = level;
        page->Add(item);
    }

    tail->next = after;
    return page;
}

void LevelsPage::Destroy(u32 flags)
{
    vtable = g_LevelsPageVTable;
    for (u32 slot = 0; slot < Slots; slot++)
    {
        if (levels[slot] != nullptr)
        {
            CallVirtual<void>(levels[slot], levels[slot]->vtable, Widget::DestroySlot, u32{DestroyAndFree});
        }

        if (rings[slot] != nullptr)
        {
            CallVirtual<void>(rings[slot], rings[slot]->vtable, Widget::DestroySlot, u32{DestroyAndFree});
        }
    }

    OlegPage::Destroy(flags);
}

MenuPage* SaveSlotsPageConstruct(void* memory, SaveManager* manager, MenuWidget* menu)
{
    auto* page = static_cast<SaveSlotsPage*>(memory);
    SaveCodePage::Construct(page, manager);
    GameController* controller = g_OlegGameController;
    page->vtable = g_SaveSlotsPageVTable;
    Widget* after = menu->next;
    // A slot for each of the save's files besides the icons
    page->count = manager->device->flags.fileCount;
    page->widgets = static_cast<SaveSlotWidget**>(MemoryAllocate2(page->count * sizeof(SaveSlotWidget*)));
    page->rings = static_cast<RingWidget**>(MemoryAllocate2(page->count * sizeof(RingWidget*)));
    Widget* tail = menu;
    for (u32 slot = 0; slot < page->count; slot++)
    {
        SaveSummary* summary = &manager->banks[slot]->summary;
        MenuItem* item = ConstructSaveCodeItem(MemoryAllocate(sizeof(SaveCodeItem)), 0, SlotItemIds + slot,
                                               SaveCodeItem::AnswerChoose, manager);
        bool even = (slot & 1) == 0;
        Vector2 place = {even ? SlotEvenX : SlotOddX, static_cast<f32>(slot) * SlotSpacing + SlotsTop};
        Vector2 scale = {1.0f, 1.0f};
        Vector2 offset = {even ? -1.0f : 1.0f, 0.0f};
        RingWidget* ring =
            RingWidget::Construct(Allocate<RingWidget>(), AnchorMiddle, PanelRings, RingSegments, RingSteps, nullptr);
        page->rings[slot] = ring;
        SaveSlotWidget* widget =
            SaveSlotWidget::Construct(Allocate<SaveSlotWidget>(), AnchorMiddle, page, item, controller, summary);
        page->widgets[slot] = widget;
        u32 colour;
        GetColor(&colour, ColourWhite);
        ring->SlideIn(colour, &place, &scale, &offset);
        GetColor(&colour, ColourWhite);
        widget->SlideIn(colour, &place, &scale, &offset);
        Vector2 radii = SlotPanelRadii;
        AddPanelRings(&radii, ring);
        tail->next = ring;
        ring->next = widget;
        tail = widget;
        page->Add(item);
    }

    tail->next = after;
    page->AddItems();
    return page;
}

void SaveSlotsPage::Destroy(u32 destroyFlags)
{
    vtable = g_SaveSlotsPageVTable;
    for (u32 slot = 0; slot < count; slot++)
    {
        if (widgets[slot] != nullptr)
        {
            CallVirtual<void>(widgets[slot], widgets[slot]->vtable, Widget::DestroySlot, u32{DestroyAndFree});
        }

        if (rings[slot] != nullptr)
        {
            CallVirtual<void>(rings[slot], rings[slot]->vtable, Widget::DestroySlot, u32{DestroyAndFree});
        }
    }

    if (widgets != nullptr)
    {
        MemoryDeallocate_(widgets);
    }

    if (rings != nullptr)
    {
        MemoryDeallocate_(rings);
    }

    // Past the save code page's destructor, straight to the menu page's
    MenuPage::Destroy(destroyFlags);
}

void InitOlegPagesModule(u32 initialize, u32 priority)
{
    if (priority != DefaultInitPriority || initialize == 0)
    {
        return;
    }

    g_OlegPagesUnusedUp.x = 0.0f;
    g_OlegPagesUnusedUp.w = 1.0f;
    g_OlegPagesUnusedShadowY = UiShadowOffset;
    g_OlegPagesUnusedUp.y = 1.0f;
    g_OlegPagesUnusedUp.z = 0.0f;
    g_OlegPagesUnusedShadowX = UiShadowOffset;
    ColourSet(&g_OlegPagesUnusedShadowColour, 0.0f, 0.0f, 0.0f, UiShadowAlpha);
    AngleFrom(&g_OlegPagesUnusedAngle, QuarterPi, AngleRadians);
    g_OlegPagesUnusedUpBack.x = 0.0f;
    g_OlegPagesUnusedUp2.w = 1.0f;
    g_OlegPagesUnusedUpBack.z = -1.0f;
    g_OlegPagesUnusedUpBack.w = 1.0f;
    g_OlegPagesUnusedUp2.x = 0.0f;
    g_OlegPagesUnusedUp2.z = 0.0f;
    g_OlegPagesUnusedUpBack.y = 1.0f;
    g_OlegPagesUnusedUp2.y = 1.0f;
    g_OlegPagesUnusedZero = 0;
    GetColor(&g_OlegPagesUnusedWhite, ColourWhite);
    GetColor(&g_OlegPagesUnusedBlack, ColourBlack);
}

void ConstructOlegPagesModule()
{
    InitOlegPagesModule(1, DefaultInitPriority);
}
