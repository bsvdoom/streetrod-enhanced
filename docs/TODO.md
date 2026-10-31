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
   vblankonként lesz új kép (55–80 FPS). Gyors próba: `--fps 0 --vsync on`.
2. **Soros fő szál**: játéklogika → EGA-felnagyítás (egy szál) → parancsok rögzítése → [párhuzamos raszter +
   resolve] → HD műszerfal (egy szál) → textúra-feltöltés 4 MB (egy szál) → megjelenítés (VSync-várakozás).
   Semmi nem fut átfedésben.
3. **A 60%-os fő szál részben pörgés**: a `host_pump` várakozása `SDL_DelayPrecise`, 1 ms-os lépésekben; a vége
   aktív várakozás. Az FPS-t nem rontja, a CPU-mérést torzítja.
4. **Felbontással növő egyszálú pixelmunka**: EGA-felnagyítás (1 M pixel res 4-en, 4 M res 8-on), HD műszerfal
   (minden kimeneti pixel, a repedés-réteg is), textúra-másolás (4 MB / 16 MB képkockánként).
5. Kisebbek: minden dolgozó szál végigmegy mind a ~10 000 parancson; az ég-lapok képkockánkénti újraolvasása;
   a HD ég-puffer (u32 mintánként) minden kitöltésnél külön memset; Proton alatt DXVK.

Lépések:

1. **Mérés**: szakaszonkénti időmérés a fő szálon (játék, EGA, rögzítés, raszter, HD, feltöltés,
   megjelenítés, várakozás), `SR_DEBUG_FPS`-sel kiírva.
2. **Ütemezés**: VSync mellett a frissítésnél nem kisebb `--fps` korlát figyelmen kívül marad (a VSync ütemez);
   a várakozás alvással megy, csak a végén pörög. A launcher „Auto” beállítása maradjon az ajánlott.
3. **Párhuzamosítás**: EGA-felnagyítás és HD műszerfal (pixelenként független) ugyanazon a szálkészleten,
   sávokra osztva.
4. **Átfedés**: a következő képkocka összerakása fusson a megjelenítés alatt (kettős puffer vagy render-szál).
5. **Hosszabb távon**: összerakás GPU-n (EGA-réteg, HD műszerfal, felnagyítás shaderrel); a CPU csak az alacsony
   felbontású rétegeket tölti fel. Maximális res/AA mellett csak így reális a 165 FPS.

## A 2026-10-31-es átvizsgálás javítandói

Lásd [AUDIT-2026-10-31.md](AUDIT-2026-10-31.md): ott a hibák és a gyors QoL-javítások listája.
