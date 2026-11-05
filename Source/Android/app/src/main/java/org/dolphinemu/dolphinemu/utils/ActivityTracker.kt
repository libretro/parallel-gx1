package org.dolphinemu.dolphinemu.utils

import android.app.Activity
import android.app.Application.ActivityLifecycleCallbacks
import android.os.Bundle

class ActivityTracker : ActivityLifecycleCallbacks {
    private val resumedActivities = HashSet<Activity>()
    private var backgroundExecutionAllowed = false
    var currentActivity : Activity? = null
        private set

    override fun onActivityCreated(activity: Activity, bundle: Bundle?) {
        currentActivity = activity
    }

    override fun onActivityStarted(activity: Activity) {}

    override fun onActivityResumed(activity: Activity) {
        currentActivity = activity
        resumedActivities.add(activity)
        if (!backgroundExecutionAllowed && !resumedActivities.isEmpty()) {
            backgroundExecutionAllowed = true
            setBackgroundExecutionAllowedNative(true)
        }
    }

    override fun onActivityPaused(activity: Activity) {
        if (currentActivity === activity) {
            currentActivity = null
        }
        resumedActivities.remove(activity)
        if (backgroundExecutionAllowed && resumedActivities.isEmpty()) {
            backgroundExecutionAllowed = false
            setBackgroundExecutionAllowedNative(false)
        }
    }

    override fun onActivityStopped(activity: Activity) {}

    override fun onActivitySaveInstanceState(activity: Activity, bundle: Bundle) {}

    override fun onActivityPostSaveInstanceState(activity: Activity, bundle: Bundle) {
        if (DirectoryInitialization.areDolphinDirectoriesReady() &&
            !activity.isChangingConfigurations
        ) {
            flushUnsavedData()
        }
    }

    override fun onActivityDestroyed(activity: Activity) {}

    companion object {
        @JvmStatic
        external fun setBackgroundExecutionAllowedNative(allowed: Boolean)

        @JvmStatic
        external fun flushUnsavedData()
    }
}
