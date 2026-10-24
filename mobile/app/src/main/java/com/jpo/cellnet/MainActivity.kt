package com.jpo.cellnet

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.viewModels
import androidx.compose.runtime.getValue
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.jpo.cellnet.ui.AppViewModel
import com.jpo.cellnet.ui.CellNetRoot
import com.jpo.cellnet.ui.CellNetTheme

class MainActivity : ComponentActivity() {

    private val viewModel: AppViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            CellNetTheme {
                val state by viewModel.state.collectAsStateWithLifecycle()
                val conversations by viewModel.conversations.collectAsStateWithLifecycle()
                val messages by viewModel.messages.collectAsStateWithLifecycle()
                val callHistory by viewModel.callHistory.collectAsStateWithLifecycle()
                CellNetRoot(
                    state = state,
                    conversations = conversations,
                    messages = messages,
                    callHistory = callHistory,
                    viewModel = viewModel
                )
            }
        }
    }
}
