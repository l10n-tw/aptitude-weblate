// -*-c++-*-
//
// This file defines the names of sections known by aptitude for pl.
//
// Due to bug #260446, double-quotes (") cannot be backslash-escaped.
// For this reason, aptitude treats adjacent pairs of apostrophese('')
// as double-quotes: that is, the string "''" in a section description
// will be rendered as one double quote.  No other characters are
// affected by this behavior.

Aptitude::Sections
{
  Descriptions {
    Unknown	"Pakiety bez określonej sekcji\n Te pakiety nie mają podanej sekcji. Być może plik ''Packages'' zawiera błąd.";
    Virtual	"Pakiety wirtualne\n Takie pakiety nie istnieją. Są to nazwy, których używają inne pakiety aby zażądać jakiejś funkcjonalności lub ją udostępnić.";
    Tasks	"Pakiety konfigurujące komputer do określonych zadań\n Pakiety w sekcji ''Zadania'' nie zawierają żadnych plików, jedynie zależą od innych pakietów. Daje to możliwość łatwego wybrania predefiniowanego zestawu pakietów dla konkretnego zadania.";

    admin	"Narzędzia administracyjne (instalacja oprogramowania, zarządzanie użytkownikami itp.)\n Pakiety w sekcji ''admin'' pozwalają na wykonywanie zadań administracyjnych takich jak instalacja oprogramowania, zarządzanie kontami użytkowników, konfigurowanie i monitorowanie systemu, badanie ruchu sieciowego, itp.";
    alien	"Pakiety skonwertowane z innych formatów (rpm, tgz, itp.)\n Pakiety w sekcji ''alien'' zostały utworzone przez program ''alien'' z pakietów w innych formatach, takich jak np. RPM.";
    cli-mono	"Mono i Common Language Infrastructure\n Pakiety w sekcji ''cli-mono'' udostępniają otwartoźródłową implementację platformy .NET firmy Microsoft, opartej na standardach ECMA do C# i Common Language Runtime. Osoby nieprogramujące w .NET nie muszą instalować samodzielnie pakietów z tej sekcji. System zarządzania pakietami zainstaluje te, które są potrzebne.";
    comm	"Programy do obsługi faksmodemów i innych urządzeń komunikacyjnych\n Pakiety w sekcji ''comm'' są używane do obsługi modemów i innych urządzeń komunikacyjnych. Znajdują się tu programy do zarządzania faksmodemami (np. PPP do połączeń wdzwanianych i programy napisane pierwotnie do tego celu np. zmodem/kermit), a także np. oprogramowanie do obsługi telefonów komórkowych, łączności z FidoNetem czy prowadzenia BBS-u.";
    database	"Serwery i narzędzia baz danych\n Pakiety w sekcji ''database'' zawierają silniki baz danych, takie jak PostgreSQL i SQLite, ich klienty oraz narzędzia do zarządzania bazami danych.";
    debug	"Symbole debugowania\n Pakiety w sekcji ''debug'' zawierają pliki potrzebne do debugowania programów. Nie ma potrzeby ich instalowania, jeśli nie chce się zdebugować danego programu.";
    devel	"Narzędzia i programy do rozwoju oprogramowania\n Pakiety w sekcji ''devel'' są używane do tworzenia nowego oprogramowania i pracy nad istniejącymi programami. Osoby niebędące programistami i niekompilujące samodzielnie oprogramowania prawdopodobnie nie potrzebują zbyt wielu programów z tej sekcji.\n .\n Należą tu kompilatory, debuggery, edytory programistyczne, programy przetwarzające źródła i inne pakiety związane z rozwojem oprogramowania.";
    doc		"Dokumentacja i programy do jej przeglądania\n Pakiety w sekcji ''doc'' dokumentują system Debian lub służą do przeglądania różnych formatów dokumentacji.";
    editors	"Edytory i procesory tekstu\n Pakiety w sekcji ''editors'' pozwalają na edycję czystego tekstu w formacie ASCII. Nie są to zwykle procesory tekstu, choć niektóre mogą nimi być.";
    electronics	"Programy do pracy z obwodami i elektroniką\n Pakiety w sekcji ''electronics'' to narzędzia do projektowania obwodów, symulatory i assemblery do mikrokontrolerów i inne programy związane z elektroniką.";
    embedded	"Oprogramowanie do systemów wbudowanych\n Pakiety w sekcji ''embedded'' przeznaczone są do pracy w systemach wbudowanych. Są to wyspecjalizowane urządzenia o znacznie mniejszej mocy niż typowe komputery biurkowe, takie jak PDA, telefony komórkowe lub Tivo.";
    fonts	"Fonty i narzędzia związane z fontami\n Pakiety w sekcji fonty zawierają fonty w wielu formatach, jak również narzędzia do zarządzania nimi.";
    games	"Gry, zabawki, programy rozrywkowe\n Pakiety z sekcji ''games'' służą głównie rozrywce.";
    gnome	"Środowisko GNOME\n GNOME stanowi zestaw programów tworzących łatwe w obsłudze, graficzne środowisko pracy. Pakiety w sekcji ''gnome'' są częścią GNOME lub są z nim silnie zintegrowane.";
    gnu-r	"GNU R do obliczeń statystycznych i wizualizacji wyników\n GNU R jest środowiskiem wolnego oprogramowania do obliczeń statystycznych i wizualizacji wyników. Pakiety w sekcji ''gnu-r'' zawierają system GNU R i wiele dodatkowych bibliotek.";
    gnustep	"Środowisko GNUstept\n GNUstep jest wieloplatformową, zorientowaną obiektowo platformą do rozwijania programów. Pakiety w sekcji ''gnustep'' są częścią platformy GNUstep lub są z nią ściśle związane.";
    graphics	"Narzędzia do tworzenia, przeglądania i edycji grafiki\n Pakiety w sekcji ''graphics'' zawierają przeglądarki plików graficznych, programy do edycji obrazu, oprogramowanie do współpracy z urządzeniami graficznymi (skanerami, kartami wideo, kamerami) i narzędzia programistyczne do obsługi grafiki.";
    hamradio	"Programy dla radioamatorów\n Pakiety w sekcji ''hamradio'' są przeznaczone głównie dla radioamatorów.";
    haskell	"Interpreter i biblioteki języka Haskell\n Pakiety w sekcji ''haskell'' zawierają interpreter języka Haskell oraz wiele dodatkowych bibliotek do wykorzystania w tym języku. Osoby nieprogramujące w Haskellu nie muszą instalować samodzielnie pakietów z tej sekcji. System zarządzania pakietami zainstaluje te, które są potrzebne.";
    httpd	"Serwery WWW i ich moduły\n Pakiety w sekcji ''httpd'' zawierają serwery WWW, których można użyć od systemów wbudowanych do największych serwerów w Internecie.";
    interpreters "Interpretery języków programowania\n Pakiety w sekcji ''interpreters'' zawierają interpretery takich języków, jak na przykład Lua, Pike, Smalltalk i Tcl oraz biblioteki do tych języków. Niektóre języki interpretowane, takie jak Perl, Python czy Ruby mają swoje własne sekcje.";
    java	"Interpreter i biblioteki języka Java\n Pakiety w sekcji ''java'' zawierają interpreter języka programowania Java oraz wiele dodatkowych bibliotek do wykorzystania w tym języku. Osoby nieprogramujące w Javie nie muszą instalować samodzielnie pakietów z tej sekcji. System zarządzania pakietami zainstaluje te, które są potrzebne.";
    kernel	"Jądro i jego moduły\n Pakiety w sekcji ''kernel'' zawierają rdzeń systemu operacyjnego. Jest to samo jądro systemu operacyjnego oraz jego moduły, które dostarczają funkcji takich jak obsługa nietypowego sprzętu lub obsługa maszyn wirtualnych.";
    kde		"Środowisko KDE\n KDE stanowi zestaw programów tworzących łatwe w obsłudze, graficzne środowisko pracy. Pakiety w sekcji ''kde'' są częścią KDE lub są z nim silnie zintegrowane.";
    libdevel	"Pliki nagłówkowe bibliotek\n Pakiety w sekcji ''libdevel'' zawierają pliki niezbędne przy budowaniu oprogramowania używającego bibliotek z sekcji ''libs''. Nie są one potrzebne, jeśli nie planuje się samodzielnie kompilować programów.";
    libs	"Biblioteki podprogramów\n Pakiety w sekcji ''libs'' dostarczają funkcji używanych przez inne programy. Z nielicznymi wyjątkami, nie powinna istnieć potrzeba samodzielnego instalowania pakietów z tej sekcji. System zarządzania pakietami zainstaluje te, które są potrzebne do spełnienia zależności.";
    lisp	"Interpreter i biblioteki języka Lisp\n Pakiety w sekcji ''lisp'' zawierają interpreter języka programowania Lisp oraz wiele dodatkowych bibliotek do wykorzystania w tym języku. Osoby nieprogramujące w Lispie nie muszą instalować samodzielnie pakietów z tej sekcji. System zarządzania pakietami zainstaluje te, które są potrzebne.";
    localization	"Pakiety językowe\n Pakiety w sekcji ''localization'' zawierają tłumaczenia programów na wiele języków.\n.\n Proszę zauważyć, że wiele programów zawiera tłumaczenia bezpośrednio w swoim pakiecie, więc jeśli nie ma odpowiedniego pakietu językowego w tej sekcji, wciąż istnieje szansa, że dany program został przetłumaczony.";
    mail	"Programy do tworzenia i przesyłania poczty elektronicznej\n Pakiety w sekcji ''mail'' zawierają programy pocztowe, demony do przesyłania poczty (MTA), oprogramowanie do obsługi list dyskusyjnych, filtry antyspamowe i inne programy związane z pocztą elektroniczną.";
    math	"Programy do analizy numerycznej i inne związane z matematyką\n Pakiety w sekcji ''math'' zawierają kalkulatory, języki do obliczeń matematycznych (podobne do pakietu Mathematica), pakiety do algebry symbolicznej i programy do wizualizacji obiektów matematycznych.";
    misc	"Różne programy\n Pakiety w sekcji ''misc'' mają zbyt nietypową funkcjonalność, aby je sklasyfikować.";
    net		"Programy do udostępniania różnych usług i do łączenia się z nimi\n Pakiety w sekcji ''net'' zawierają klienty i serwery do wielu protokołów, narzędzia do manipulowania protokołami niskiego poziomu i ich debugowania, komunikatory i inne programy sieciowe.";
    news	"Klienty i serwery grup dyskusyjnych\n Pakiety w sekcji ''news'' związane są z systemem grup dyskusyjnych Usenetu. Zawierają czytniki i serwery grup dyskusyjnych.";
    ocaml	"Interpreter i biblioteki języka Ocaml\n Pakiety w sekcji ''ocaml'' zawierają interpreter języka programowania Ocaml oraz wiele dodatkowych bibliotek do wykorzystania w tym języku. Osoby nieprogramujące w Ocamlu nie muszą instalować samodzielnie pakietów z tej sekcji. System zarządzania pakietami zainstaluje te, które są potrzebne.";
    oldlibs	"Stare wersje bibliotek\n Pakiety w sekcji ''oldlibs'' są przestarzałe i nie powinny być używane przez nowe programy. Są udostępnione dla zachowania zgodności wstecz lub ponieważ są ciągle wymagane przez inne pakiety Debiana.\n .\n Z nielicznymi wyjątkami, nie powinna istnieć potrzeba samodzielnego instalowania pakietów z tej sekcji. System zarządzania pakietami zainstaluje te, które są potrzebne do spełnienia zależności.";
    otherosfs	"Emulatory i programy do odczytu obcych systemów plików\n Pakiety w sekcji ''otherosfs'' emulują różny sprzęt i systemy operacyjne oraz zawierają narzędzia do przenoszenia danych między różnymi systemami operacyjnymi i platformami sprzętowymi (na przykład do odczytu dyskietek DOS-a, lub komunikacji z Palm Pilotami).\n .\n Należy zauważyć, że programy do nagrywania płyt CD także znajdują się w tej sekcji.";
    perl	"Interpreter i biblioteki języka Perl\n Pakiety w sekcji ''perl'' zawierają interpreter języka programowania Perl oraz wiele dodatkowych bibliotek do wykorzystania w tym języku. Osoby nieprogramujące w Perlu nie muszą instalować samodzielnie pakietów z tej sekcji. System zarządzania pakietami zainstaluje te, które są potrzebne.";
    php		"Interpreter i biblioteki języka PHP\n Pakiety w sekcji ''php'' zawierają interpreter języka programowania PHP oraz wiele dodatkowych bibliotek do wykorzystania w tym języku. Osoby nieprogramujące w PHP nie muszą instalować samodzielnie pakietów z tej sekcji. System zarządzania pakietami zainstaluje te, które są potrzebne.";
    python	"Interpreter i biblioteki języka Python\n Pakiety w sekcji ''python'' zawierają interpreter języka Python oraz wiele dodatkowych bibliotek do wykorzystania w tym języku. Osoby nieprogramujące w Pythonie nie muszą instalować samodzielnie pakietów z tej sekcji. System zarządzania pakietami zainstaluje te, które są potrzebne.";
    ruby	"Interpreter i biblioteki języka Ruby\n Pakiety w sekcji ''ruby'' zawierają interpreter języka programowania Ruby oraz wiele dodatkowych bibliotek do wykorzystania w tym języku. Osoby nieprogramujące w Rubym nie muszą instalować samodzielnie pakietów z tej sekcji. System zarządzania pakietami zainstaluje te, które są potrzebne.";
    science	"Oprogramowanie do pracy naukowej\n Pakiety w sekcji ''science'' zawierają narzędzia dla astronomów, biologów, chemików oraz inne programy związane z nauką.";
    shells	"Interpretery poleceń oraz alternatywne środowiska terminalowe\n Pakiety w sekcji ''shells'' zawierają programy zapewniające interfejs wiersza poleceń do systemu.";
    sound	"Narzędzia do odtwarzania i nagrywania dźwięku\n Pakiety w sekcji ''sound'' zawierają programy do odtwarzania i nagrywania muzyki oraz kodowania jej do wielu formatów, miksery, narzędzia do kontroli głośności, sekwencery MIDI, programy do tworzenia zapisu nutowego, sterowniki urządzeń dźwiękowych i programy do przetwarzania dźwięku.";
    tex		"System składu TeX\n Pakiety w sekcji ''tex'' związane są z systemem TeX, służącym do wysokiej jakości składu dokumentów. Zawierają sam system TeX, jego dodatkowe pakiety, edytory zaprojektowane do TeX-a, narzędzia do konwersji plików źródłowych i wynikowych TeX-a do różnych formatów, fonty oraz inne programy związane z TeX-em.";
    text	"Narzędzia do przetwarzania tekstu\n Pakiety w sekcji ''text'' zawierają filtry tekstowe, korektory pisowni, programy słownikowe, narzędzia do konwersji kodowań znaków i formatów tekstu (np. Unix--DOS), programy formatujące tekst oraz inne programy do obróbki zwykłego tekstu.";
    utils	"Różne narzędzia systemowe\n Pakiety w sekcji ''utils'' zawierają narzędzia o przeznaczeniu zbyt specyficznym aby je sklasyfikować.";
    video	"Narzędzia do nagrywania, odtwarzania, edytowania i strumieniowania wideo\n Pakiety w sekcji ''video'' zawierają programy do odtwarzania plików wideo, DVD i TV oraz obróbki wideo. Używając tych narzędzi jest możliwe stworzenie kompletnego studia do obróbki wideo lub domowego centrum multimediów.";
    vcs		"Systemy kontroli wersji\n Pakiety w sekcji ''vcs'' zawierają programy do zarządzania zmianami w dokumentach, programach i innych plikach komputerowych, umożliwiając łatwe odtworzenie starych wersji lub śledzenie poszczególnych linii rozwoju.";
    web		"Przeglądarki, serwery i inne narzędzia WWW\n Pakiety w sekcji ''web'' zawierają przeglądarki i serwery WWW, serwery pośredniczące, programy do tworzenia skryptów CGI lub aplikacji opartych o WWW, gotowe aplikacje i inne programy związane z WWW.";
    x11		"System okien X i związane z nim programy\n Pakiety w sekcji ''x11'' to podstawowe pakiety systemu okien X, menedżery okien, narzędzia do systemu X i inne programy z graficznym interfejsem użytkownika w systemie X, umieszczone tutaj, ponieważ nie pasowały nigdzie indziej.";
    xfce	"Środowisko Xfce\n Xfce stanowi zestaw programów tworzących łatwe w obsłudze, graficzne środowisko pracy. Pakiety w sekcji ''xfce'' są częścią Xfce lub są z nim silnie zintegrowane.";
    zope	"Platforma Zope/Plone\n Pakiety w sekcji ''zope'' zawierają serwer aplikacji do budowania systemów zarządzania treścią, intranetów, portali i dostosowanych aplikacji. Jednym z takich systemem zarządzania treścią zbudowanym na Zope jest Plone.";

    main	"Główne archiwum Debiana\n Dystrybucja Debiana składa się z pakietów z sekcji ''main''.Każdy pakiet w tej sekcji jest wolnym oprogramowaniem.\n .\n Aby uzyskać więcej informacji o tym co Debian uważa za wolne oprogramowanie należy przeczytać http://www.debian.org/social_contract#guidelines";
    contrib	"Programy zależne od oprogramowania spoza Debiana\n Pakiety w sekcji ''contrib'' nie należą do systemu Debian.\n .\n Te pakiety zawierają wolne oprogramowanie, ale zależą od programów niezawartych w Debianie. Programy te mogą nie być wolnym oprogramowaniem i znajdować się w sekcji ''non-free'', ich dystrybucja przez Debiana może być niedozwolona, lub (rzadko) mogą nie być jeszcze stworzone ich pakiety do Debiana. .\n Aby uzyskać więcej informacji o tym co Debian uważa za wolne oprogramowanie należy przeczytać http://www.debian.org/social_contract#guidelines";
    non-free	"Programy niebędące wolnym oprogramowaniem\n Pakiety w sekcji ''non-free'' nie należą do systemu Debian.\n .\n Te pakiety nie spełniają jakichś wymagań ''Wytycznych Debiana dotyczących wolnego oprogramowania'' (patrz niżej). Aby mieć pewność, że wolno używać programów z tej sekcji w zamierzony sposób, należy zapoznać się z ich licencjami.\n .\n Aby uzyskać więcej informacji o tym co Debian uważa za wolne oprogramowanie, należy przeczytać http://www.debian.org/social_contract#guidelines";
    non-US	"Programy przechowywane poza USA z powodu ograniczeń eksportowych\n Pakiety w sekcji ''non-US'' zwykle zawierają procedury kryptograficzne i z tego powodu nie mogą być eksportowane z USA. Niektóre zaś implementują opatentowane algorytmy. Z tych powodów przechowywane są one na serwerze w ''wolnym świecie''.\n .\n Uwaga: Debian, po zasięgnięciu opinii prawnej na temat niedawnych zmian w przepisach eksportowych, przenosi obecnie oprogramowanie kryptograficzne do głównego archiwum. Większość pakietów znajdujących się kiedyś w tej sekcji jest więc teraz dostępnych w sekcji ''main''.";
  };
};

