/* Re-enable features that HB_TINY disables but the
 * raster / vector / gpu pipelines need. */

#undef HB_NO_DRAW         /* outline drawing for vector / raster / gpu */
#undef HB_NO_METRICS      /* glyph advances + extents */
#undef HB_NO_COLOR        /* COLR palette / layers */
#undef HB_NO_PAINT        /* paint walk used by vector / gpu paint */
#undef HB_NO_AAT          /* AAT shapers for Apple font tech (morx, kerx, trak) */
#undef HB_NO_NAME         /* hb_ot_name_get_utf8 for the font picker label */
#undef HB_NO_VAR          /* hb_font_set_variations, fvar axes */
#undef HB_NO_OT_FONT_GLYPH_NAMES  /* hb_font_glyph_to_string for shape table */
