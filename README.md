# Neverlose Loader — complete native launcher

Нативный C++20 лаунчер без Qt и .NET для Minecraft 1.21 + Fabric.

## Полная реализация

- загружает официальный Minecraft 1.21 через Mojang version manifest;
- загружает client JAR, библиотеки, Windows natives, asset index и все игровые assets;
- проверяет Mojang-файлы по SHA-1;
- получает последнюю стабильную версию Fabric Loader из Fabric Meta;
- загружает и проверяет Fabric Maven-библиотеки;
- получает Fabric API, Iris, Sodium и Lithium через Modrinth API;
- фильтрует Modrinth-файлы по `fabric` и `1.21`;
- проверяет моды по SHA-512 из ответа Modrinth;
- поддерживает дополнительные GitHub/CDN artifacts с SHA-256;
- повторно использует корректные файлы и скачивает только отсутствующие/повреждённые;
- повторяет временно неудачные сетевые запросы и ограничивает размер ответа;
- удаляет только устаревшие JAR, ранее установленные самим лаунчером;
- распаковывает native libraries;
- собирает classpath и запускает Fabric KnotClient;
- использует локальный offline-профиль и детерминированный UUID;
- ищет Java в `runtime/bin`, `JAVA_HOME`, PATH и runtime официального Minecraft Launcher.

## Профиль

При первом запуске появляется onboarding:

- предлагается ник Windows, который можно изменить;
- введённый ник сохраняется и передаётся Minecraft как `--username`;
- можно выбрать PNG/JPG/BMP-аватар;
- аватар отображается круглым через Direct2D geometry mask;
- настройки сохраняются в `%LOCALAPPDATA%/NeverloseLoader/launcher.ini`.

Offline-профиль подходит для локальной игры и offline-mode серверов. Для premium online-mode серверов требуется отдельная реализация официального Microsoft OAuth.

## UI и анимации

- анимированный ambient glow;
- мягкое появление onboarding;
- пульсирующая подсветка NL-логотипа и аватара;
- пульсация online-индикатора;
- плавная реакция навигации, пресетов, карточек и кнопок;
- fade-переходы страниц без сдвига hitbox;
- shimmer кнопки запуска и компактный вращающийся индикатор загрузки;
- анимированный прогресс установки;
- отмена текущей операции через Esc;
- оригинальный логотип из букв NL;
- векторные Direct2D-иконки и 11 SVG-исходников.

## Сборка

Установи Visual Studio workload `Desktop development with C++`, Windows SDK и CMake Tools. Затем:

```text
build-release.bat
```

Результат:

```text
build\Release\NeverloseLoader.exe
```

Подробная публикация и настройка удалённого manifest описана в `GITHUB_SETUP_RU.md`.

## Dark Blue UI v2

Интерфейс полностью переработан в компактной тёмно-синей стилистике: широкий navigation rail, hero-карточка сборки, status/runtime панели, двухколоночные компоненты и единая страница настроек. Используется нативный Windows graphics stack **Direct2D + DirectWrite + WIC + DWM**, без Qt/.NET. NL-логотип встроен в EXE как `RCDATA`, поэтому он отображается внутри интерфейса даже без внешней папки assets; `.ico` отдельно встроен как Windows icon resource.

## Server components and motion system

В локальный manifest добавлены 8 Fabric-компонентов: Fabric API, Iris, Sodium, Lithium, Mod Menu, FerriteCore, ImmediatelyFast и Entity Culling. Каждый компонент разрешается через Modrinth API по `modrinthProject`, скачивается с сервера и проверяется SHA-512. Удалённый manifest может добавлять новые компоненты без пересборки EXE; список поддерживает плавную прокрутку.

Motion-система включает fade страниц и первого появления, два фоновых glow-слоя, hover-переходы навигации/профиля/оконных кнопок/пресетов/карточек, пружинные toggle-переключатели, lift иконок, shimmer запуска, spinner и progress, анимированные кнопки Settings, тяжёлый RAM slider и инерционную прокрутку списка. Геометрия hitbox остаётся неподвижной.
