package com.watermellonstudios.audio.harness

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent

/** Shell de Android. Todo lo interesante vive en [HarnessApp], en commonMain. */
class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent { HarnessApp() }
    }
}
