# KB2: keyboard and gamepad focus audit of every menu page

Branch `kb2` (from c817179), 2026-11-25. Builds on KB1 (`docs/tasks/KB1-menu-focus.md`). Every check is text
evidence from the running game: the focus snapshot (`window.nfs5Focus()`), the game's own fields read from guest
memory, and the game log. No screenshots.

## Tools

- `web/tools/focus_test.mjs`: the snapshot reader and the input routing on fake guest memory (unit checks).
- `web/tools/focus_drive.mjs`: one headless game with a fake pad and an HTTP control port (`focus`, `tree`, `key`,
  `pad`, `cmd`, `click`, `drag`, `eval`, `log`); `--players 2` runs two games into one online race; `--env` for
  NETTEST races.
- `web/tools/focus_walk.mjs`: a keyboard-only walk. For each page it asserts that every focus item is reachable from
  the first with the arrows, that there is a way back, that every combo / list reaches its last row and closes on Esc,
  and it lists visible controls the focus layer does not offer. It enters pages with arrows + Enter and leaves with Esc;
  `--start 'a > b'` walks one subtree, `--deny` skips commands that spend money, start races or delete things.
  Example: `node web/tools/focus_walk.mjs --port 8774 --start "button@152,47 > SETTYPEKNOCKOUT" --depth 4`.
  The walk dedups pages by their item layout, so Quick Race / Quick Knockout (same layout) are walked once.

## The four user bugs

| # | Bug | Fix | Proof |
|---|-----|-----|-------|
| 1 | Online Escape menu has no usable buttons; no pad way to resume or quit | The network race's leave notice (HInfo `0x005B2BFC`, game type `[0x006573E8]==3`) gets two buttons of the focus layer's own on the box's footer: "Back to the race" (sends Space) and "Leave the race" (sends Esc); clickable, tappable and focusable. Pad B / Start go back to the race. A key the game saw pressed is released to it even when the focus layer swallows the release (ad2c65a) | Two games online (`focus_drive --players 2`): pad Start -> mode 1, 2 items in the footer strip (y 319 of 480); pad B -> driving again; Start/Start toggles the notice; d-pad right x2 -> "Leave the race", A -> mode 0, game state 3, the server log says `game #100: host leaving the race` |
| 2 | Quick Race difficulty dropdown cannot pick an option or close | Open combos step row by row through the game's own hover test (`readRows`: open area, the accessor's row height, first visible row, scroll bar only when `+0x1DC`), Enter picks the hovered row, Esc closes (ad2c65a). Combos are clicked right of their caption (`+0x1F0`, 45dada8) | Quick Race: Enter opens, Down moves the hovered row, Enter picks it and the list closes, Esc closes without a change. Re-checked 2026-11-25 (see "Re-checks" below) |
| 3 | The LAST option of the car and track combos cannot be selected | Same row stepper; at the edge of the shown rows it clicks the scroll arrow and returns the cursor; rows of other heights (car lists) are stepped by a third of a row until the hover index moves (ad2c65a) | Track combo: selected index 22 of 23; merged car list: 14 of 15 (quick race), 86 of 87 (knockout car compare, walk 2026-11-25, `hover 86, count 87, closed`) |
| 4 | The bottom bar of the race results is outside the focus | `pu_pad_mode` returns 1 (race menu) while `[0x00657424]==2` (race over) and the mouse is mapped into the HUD layout there (1558a62); the replay bar's 7 slots (HReplay `[0x0060CA58]`, `+0x12C..+0x15C`) are focus items under and after the result pages; result pages close with Enter / pad A (sent as Space) | Quick race (NETTEST, AI drives): 7 slots focusable in mode 1; the "results" slot reopened [HRaceSummary, HTrackRecords]; pad A and Enter closed them; the "save" slot opened HReplaySave (save / cancel); B cancelled; a name typed with the pad keyboard ("K") + save wrote `K.rpl` |

## Page inventory

Status: **OK** = walked or checked, nothing to fix; **fixed** = was broken, fixed on kb2; **open** = still not right
(why); **not reached** = needs a game state the scripts cannot make cheaply; the page uses only control classes that are
checked elsewhere. Page names are the layout files (`FEData/Layouts/*.lay`); `button@x,y` is how the walk names a
button without a command.

### Frontend

| Page | Status | Notes |
|------|--------|-------|
| selectname (profile) | fixed | Its only way back is "choose" (SETNAMETHENBACK): Esc / pad B take it now (cff1e27). Remove is excluded while disabled (flag 0x10) |
| createplayer | OK | Name field + pad keyboard ("Kb" typed, read back from the field), B closes the keyboard, then CREATEPLAYERDONE shows; Esc = CANCELCREATEPLAYER |
| GameSetup (main menu) | OK | 8 arc buttons + bar; all reachable (arrow graph checked offline); Esc opens the game's quit dialog |
| Quit dialog (FEDialog yes/no) | fixed | Dialogs are a focus page (ad2c65a); the negative button is found by its caption 0x2BF (cff1e27); pad down x2 + A = "No"; Esc closes |
| credits | OK | back GOBACK |
| chronicle | fixed | Year slider (FEHorizontalSlider) is now a focus slider: Left/Right drag the thumb one cell (value 0->1->2->3->4->3 checked, ba37f3c). Model buttons, movie box OK. The main picture box (FEChronicleMainBoxControl) is not offered: its click opens chroniclepicture / chroniclemedia only when the CD media exist, and the web data has no `FEData/chron` at all (log: `CANNOT OPEN .\fedata\chron\icons*.fsh`) |
| chroniclemoviemain | OK | 3 items, back GOBACK |
| Options (+ Audio, Display, Graphics, Controllers tabs) | OK | 21 items, all combos reach the last row and close (`197,356`: 14 of 14 ...) |
| cdplayer (playlist panel) | OK | 13 items, HIDEPLAYLISTPANEL; Esc after it follows the game's page history (same as the mouse) |
| cameras tab | fixed | The 4 camera pictures (FECameraPicture) were mouse-only; now focus buttons, Enter changes the same flags as a mouse click (0f725bb). Camera select buttons OK |
| Controllers / key binding / dead zone | OK (KB1) | binding capture checked in KB1 |
| singleplayer | OK | 5 items, difficulty combo 3 of 3 rows |
| quickrace / quickknockout (same layout) | OK | 12 items; all combos reach the last row; car compare reachable |
| knockout | OK | 11 items incl. the colour box; combos 8/8, 7/7, 15/15 |
| locationsetup (knockout tracks) | OK | 14 items incl. KNOCKOUTTRACKINC/DEC and the track list control (FEKnockoutTrackListControl, now a focus control); list 8 of 8 |
| location / locationQK / locationMP | OK | track combos reach the last row (bug 3) |
| trackrecords | OK | 5 items, back GOBACK (RESETRECORDS skipped by the walk on purpose) |
| settings (race settings) | fixed | In the factory driver one combo's caption is 95 of 162 px wide: the centre click landed on the caption and the game ignored it (FEComboBox 004C6AF0 opens only right of `+0x1F0`). Combos are now clicked in their value area (45dada8); all 4 open and reach their last row |
| carcompare | OK | merged car list 86 of 87, closes on Esc |
| garage (quick race) | OK | settings / car setup / paint shop / car combo |
| paintshop | OK | 2 combos (decal 8 of 8), 4 colour boxes, CHANGEDECAL |
| carsetup | OK | 19 +/- buttons (FECarSettingButton): + raised a value 20 -> 21; the 5 small sliders (FEHorizontalSlider with accessor 0x005B4F44) are not offered, their +/- buttons do the same; SAVE/LOADSETTINGS do nothing visible here with the mouse either |
| tournament (career hub) | OK | 7 items, the hidden "n/a" combo (flag 0x10) is excluded |
| careerfirst / era movie | OK | movie page with only OPTIONS/QUIT: Esc skips it (game behaviour) |
| garage (career, no car yet) | OK | dealership / used cars |
| dealership | OK | 12 items (INC/DEC, colour boxes, BUYCAR, test drive, showcase, compare); "not enough credits" message box: Esc / B closes it |
| usedcars | OK | empty at career start (all buttons hidden, nothing focusable but the bar) |
| testdriveoval | OK | 3 items |
| showcase | OK | 7 items |
| prerace | OK | 7 items (TOURNREMAININC/DEC) |
| repair / advancedrepair / parts / partsdesc / carsetupdesc (career, owned car) | not reached | needs a bought car; the start money buys no new 356 and the used market is empty. Same control classes as the pages above (GOTOBUTTON, list box, combos) |
| factwelcome | fixed | A modal one-button message over a hidden bottom bar: Esc / pad B had no way on. A page with a single button and nothing else now takes Esc / B as that button (fec4d34) |
| missionbrief / missionmap | OK | GOBACK / GOBACKMINUSONE |
| factorydriver (hub) | OK | 7 items |
| teamprofile | OK | 3 items |
| factfirst / firstlicense / factdone / levelvictory / levelbadge / FactoryVictory / VictoryRoom / VictoryMessage / endofera | not reached | after-race / win states; one-button or GOTOBUTTON pages, covered by the one-button rule and the generic reader |
| loadreplay | OK | list + GOBACK (STARTREPLAY / KILLREPLAY hidden with no replays) |
| protocol | n/a | the web build goes from SETTYPEMULTIPLAYER straight to accountlogin |
| accountlogin | OK | 2 input boxes, SIGNON, CLEARACCOUNTINFO, REGISTERUSER, GOBACK |
| serverlist | OK | list + ENTER |
| udpip (online lobby) | OK | CREATE, chat input, 2 lists, bar |
| create-game dialog (FEDialogInputCancel) | fixed | Its name field is a text field now (pad keyboard types into it) and its Cancel is found by caption although it is listed first (cff1e27); OK created the game |
| creategame | OK | 3 combos reach the last row (33/33, 3/3, 84/84), colour box, EJECTPLAYER, chat, list |
| joingame / settingsMP | not reached | needs a second player in the lobby; same control classes as creategame |
| local / modem / peertopeerip | n/a | not used by the web build |

### Race pages

| Page | Status | Notes |
|------|--------|-------|
| HPause | OK | Continue resumes, Restart restarts (mode 0 -> 2), Quit Race opens HExit |
| HExit | OK | view replay / exit to game menu / cancel; pad B returns to HPause |
| HYesNoOverlay | OK | read like the other pages (visible HButtonVisual) |
| HReplaySave | fixed | name field + pad keyboard, save writes `<name>.rpl`, B cancels (bug 4 proof) |
| HInfo (network leave notice) | fixed | bug 1 |
| HInfo (single player) | OK | 0 items, Enter / A = Space closes |
| HRaceSummary / HTrackRecords / HCongrats / HTournamentStandings / HMissionSummary | fixed | Enter / pad A close them (Space) |
| Replay bar (HReplay) | fixed | bug 4 |
| In-race Options (`[0x00606AA4]`) | n/a | no race page returns code 1 in this build (HPause returns 2 / 3 / 0x1A only), so the frontend options never open inside a race |

## Re-checks 2026-11-25

(filled in below as they run)

## Risks and limits

- Long lists with the pad: one row per press, held d-pad repeats every 80 ms after 400 ms; the 87-row car list takes
  about 8 s held.
- FECar3DControl (click the car to open doors / boot) stays mouse-only: decorative, the hit part depends on where the
  car is clicked.
- The walk's own timing: pages that build in steps (bottom bar animation) can still give a false "Esc did not lead
  back"; each such report was re-checked by hand (dealer, garage, knockout car compare: Esc works).
