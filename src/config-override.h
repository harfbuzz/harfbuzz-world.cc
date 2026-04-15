/* Re-enable features that HB_TINY disables but the
 * raster / vector / gpu pipelines need. */

#undef HB_NO_DRAW         /* outline drawing for vector / raster / gpu */
#undef HB_NO_METRICS      /* glyph advances + extents */
#undef HB_NO_COLOR        /* COLR palette / layers */
#undef HB_NO_PAINT        /* paint walk used by vector / gpu paint */
