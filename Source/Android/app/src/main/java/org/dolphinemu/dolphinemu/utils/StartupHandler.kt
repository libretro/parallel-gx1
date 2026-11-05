// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.utils

import android.content.ClipData
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.text.TextUtils
import androidx.fragment.app.FragmentActivity
import org.dolphinemu.dolphinemu.activities.EmulationActivity

object StartupHandler {
    @JvmStatic
    fun HandleInit(parent: FragmentActivity) {
        // Set up and/or sync Android TV channels
        if (TvUtil.isLeanback(parent)) {
            TvUtil.scheduleSyncingChannel(parent)
        }

        val gamesToLaunch = getGamesFromIntent(parent.intent)
        if (gamesToLaunch != null && gamesToLaunch.isNotEmpty()) {
            // Start the emulation activity, send the ISO passed in and finish the main activity
            EmulationActivity.launch(parent, gamesToLaunch, false, true)
        }
    }

    private fun getGamesFromIntent(intent: Intent): Array<String>? {
        // Priority order when looking for game paths in an intent:
        //
        // Specifying multiple discs (for multi-disc games) is prioritized over specifying a single
        // disc. But most of the time, only a single disc will have been specified anyway.
        //
        // Specifying content URIs (compatible with scoped storage) is prioritized over raw paths.
        // The intention is that if a frontend app specifies both a content URI and a raw path, newer
        // versions of Dolphin will work correctly under scoped storage, while older versions of Dolphin
        // (which don't use scoped storage and don't support content URIs) will also work.

        // 1. Content URI, multiple
        val clipData: ClipData? = intent.clipData
        if (clipData != null) {
            val uris = Array(clipData.itemCount) { i ->
                clipData.getItemAt(i).uri?.toString() ?: "null"
            }
            return uris
        }

        // 2. Content URI, single
        val uri: Uri? = intent.data
        if (uri != null) {
            return arrayOf(uri.toString())
        }

        val extras: Bundle? = intent.extras
        if (extras != null) {
            // 3. File path, multiple
            val paths = extras.getStringArray("AutoStartFiles")
            if (paths != null) {
                return paths
            }

            // 4. File path, single
            val path = extras.getString("AutoStartFile")
            if (!TextUtils.isEmpty(path)) {
                return arrayOf(path!!)
            }
        }

        // Nothing was found
        return null
    }
}
