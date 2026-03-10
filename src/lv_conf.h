/**
 * @file lv_conf.h
 * Configuration file for LVGL.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*
 * COPY THIS FILE AS lv_conf.h
 */

#if 1 /*Set it to "1" to enable the content*/

/*******************
 * GENERAL SETTING
 *******************/
#define LV_HOR_RES_MAX          (320)
#define LV_VER_RES_MAX          (240)
#define LV_COLOR_DEPTH          16
#define LV_TICK_CUSTOM          0

/*********************
 *      FONTS
 *********************/

/*Montserrat fonts with ASCII range and some symbols using bpp = 4
 *https://fonts.google.com/specimen/Montserrat*/
#define LV_FONT_MONTSERRAT_8     1
#define LV_FONT_MONTSERRAT_14    1
#define LV_FONT_MONTSERRAT_16    1
#define LV_FONT_MONTSERRAT_20    0
#define LV_FONT_MONTSERRAT_24    0
#define LV_FONT_MONTSERRAT_28    0
#define LV_FONT_MONTSERRAT_30    1
#define LV_FONT_MONTSERRAT_44    1

/*Set LV_FONT_DEFAULT as default font*/
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/****************
 * WIDGETS
 ****************/
#define LV_USE_ARC        1
#define LV_USE_BAR        1
#define LV_USE_BTN        1
#define LV_USE_BTNMATRIX  1
#define LV_USE_CANVAS     1
#define LV_USE_CHECKBOX   1
#define LV_USE_DROPDOWN   1
#define LV_USE_IMG        1
#define LV_USE_LABEL      1
#define LV_USE_LINE       1
#define LV_USE_ROLLER     1
#define LV_USE_SLIDER     1
#define LV_USE_SWITCH     1
#define LV_USE_TEXTAREA   1
#define LV_USE_TABLE      1

/****************
 * OTHERS
 ****************/
/*1: Enable API functions LV_OBJ_FLAG_LAYOUT_1/2*/
#define LV_USE_API_EXTENSION_V7  1

#endif /*LV_CONF_H*/

#endif /*End of "if 1"*/
