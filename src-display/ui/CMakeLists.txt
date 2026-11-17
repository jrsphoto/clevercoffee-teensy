SET(SOURCES screens/ui_ScreenReadout.c
    screens/ui_ScreenHome.c
    screens/ui_ScreenSetup.c
    screens/ui_ScreenDisplay.c
    screens/ui_ScreenBrew.c
    screens/ui_ScreenWifi.c
    screens/ui_ScreenAbout.c
    ui.c
    components/ui_comp_hook.c
    ui_helpers.c
    images/ui_img_cup_png.c)

add_library(ui ${SOURCES})
