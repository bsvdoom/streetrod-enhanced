# Megvalósítandó

## FPS: 165 Hz elérése (megbeszélve 2026-10-31, tesztelés másnap)

Mért helyzet: Linuxon `--res-scale 4 --aa 1 --draw-distance 120 --hd on --fps 165 --vsync on` mellett kb. 50 FPS
165 Hz-es monitoron; Windowson (Lutris/Proton) is a vártnál kevesebb. Futás közben egy szál 60% fölött, a többi
10% alatt: nem a párhuzamos raszterizálás a szűk keresztmetszet, hanem a fő szál soros munkája és az ütemezés.

Fejetlen mérés (`SR_ENH_TRACE`, res-scale 4, aa 2, 12 mag): összerakás kb. 6 ms (út 0.4, raszter + resolve
4.0, HD műszerfal 1.6), plusz EGA-felnagyítás, textúra-feltöltés, megjelenítés.

Valószínű okok, fontossági sorrendben:

1. **`--fps 165` + `--vsync on` együtt**: explicit korlátnál az időzítős ütemezés és a VSync is vár. A két óra
   nincs fázisban (165,000 Hz vs. a monitor valódi frissítése), a lekésett vblank egész ciklust vár, így 2–3
   vblankonként lesz új kép (55–80 FPS). **Javítva:** VSync mellett a frissítésnél nem kisebb korlát figyelmen
   kívül marad.
2. **Soros fő szál**: játéklogika → EGA-felnagyítás (egy szál) → parancsok rögzítése → [párhuzamos raszter +
   resolve] → HD műszerfal (egy szál) → textúra-feltöltés 4 MB (egy szál) → megjelenítés (VSync-várakozás).
   Semmi nem fut átfedésben.
3. **A 60%-os fő szál részben pörgés**: a `host_pump` várakozása `SDL_DelayPrecise`, 1 ms-os lépésekben; a vége
   aktív várakozás. Az FPS-t nem rontja, a CPU-mérést torzítja.
4. **Felbontással növő egyszálú pixelmunka**: EGA-felnagyítás (1 M pixel res 4-en, 4 M res 8-on), HD műszerfal
   (minden kimeneti pixel, a repedés-réteg is), textúra-másolás (4 MB / 16 MB képkockánként).
5. Kisebbek: minden dolgozó szál végigmegy mind a ~10 000 parancson; az ég-lapok képkockánkénti újraolvasása;
   a HD ég-puffer (u32 mintánként) minden kitöltésnél külön memset; Proton alatt DXVK.

Második mérés (felhasználó, maximális beállítás): 40–42 FPS VSync-kel, 50 VSync nélkül; egy szál 70–80%, a
többi kb. 33%. Fejetlen mérés res-scale 8, AA 2 (2560 × 1600, 6,9 M minta), látótáv 120, 12 mag:

| | előtte | utána |
|---|---|---|
| raszter + resolve | 15,9 ms | 4,9 ms |
| HD műszerfal | 7,0 ms | 1,9 ms |
| képkocka (VSync nélkül) | 28 ms, 36 FPS | 12,3 ms, 81 FPS |

res-scale 4 + AA 4 (ugyanannyi minta): kb. 120 FPS.

Lépések:

1. **[kész] Ég-sorok:** egy eredeti képsor res × AA mintasora azonos; eddig mind mintánként (osztás, `floor`,
   maradék, két képből) számolódott, és ez a felső sávokat kapó szálakra esett (12 ms vs. 0,8 ms). Most az
   azonos sort másolja (`render_sky_band`).
2. **[kész] HD ég-puffer csak az ég-sorokban:** soronkénti jelző; a talaj-sorokban nincs törlés és olvasás.
3. **[kész] HD műszerfal és repedés-réteg párhuzamosan:** `enh_parallel_rows` a szálkészleten, sávokra osztva.
4. **[kész] A dolgozó szálak az `SDL_Quit` előtt állnak le** (`host_set_shutdown_handler`): egy t2-futás egyszer
   szegmentálási hibával ért véget kilépéskor; nem reprodukálható, de az `atexit` az `SDL_Quit` után futott.
5. **Maradék a fő szálon** (res 8): EGA-felnagyítás kb. 1 ms, textúra-feltöltés 1,7 ms, megjelenítés 2,1 ms
   (fejetlen, szoftveres renderer), parancsok rögzítése 0,5 ms. Következő lépések: EGA-felnagyítás
   párhuzamosan; a várakozás alvással (csak a végén pörögjön); átfedés (a következő képkocka a megjelenítés
   alatt); hosszabb távon összerakás GPU-n.

## HD műszerfal: későbbi javítások (felhasználói teszt, 2026-10-31)

- **Kormány:** az elfordított állások (`wheel_left/right.png`) még nem az igaziak; csak a küllők egy része
  forog, az alsó ív nyoma ott marad.
- **Motorháztető:** még nem az igazi (a HD motorháztető alakja/átszínezése).
- **Váltó:** a HD váltólap alatt ott maradt a régi (EGA) váltódoboz is, és mintha más felbontásban lenne;
  automata és kézi váltónál is (képernyőkép: a HD lap balra-fent, mögötte a régi kulissza és gomb).
- **A végén mindent minden felbontáson ellenőrizni** (res-scale 2..8, AA 1..4).

## Nyitott tételek az átvizsgálásból

Lásd [AUDIT-2026-10-31.md](AUDIT-2026-10-31.md): a motorháztető-átszínezés heurisztikája (alacsony
prioritás).

## Később: minden kép HD-ben (felhasználói terv, 2026-10-31)

Az összes eredeti kép cseréje HD-re, a műszerfal mintájára (`assets/hd`, `--hd on`):

- Bob's Drive-In
- benzinkút
- garázs
- az ellenfelek autói (a választó- és bemutatóképek)
- az autók verseny közben (ellenfél, saját autó nézetei)
- rendőrautók
- és a többi képernyő (menük, boltok, dialógusok stb.)

A festék palettaregiszter, ezért az autóképek semleges színben, festékmaszkkal készülnek, és futás közben
színeződnek (lásd [hd-cockpit-design.md §5.3, §9](superpowers/specs/2026-10-30-hd-cockpit-design.md)).

## Később: Retry-ötletek

Lásd [HANDOFF-retry.md](superpowers/HANDOFF-retry.md), „Ideas”: King-büntetés Retry esetén, Retry slot
választása a launcherben (`--retry-slot`).
