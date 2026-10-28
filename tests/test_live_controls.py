import logging
import os
import threading
import time
import unittest
from types import SimpleNamespace
from unittest.mock import patch, Mock

@unittest.skipUnless(os.name == 'nt', 'Windows dependencies')
class LiveControlTests(unittest.TestCase):
    def app(self):
        from localstt.tray_app import LocalSTTTrayApp
        from localstt.config import AppConfig
        app=LocalSTTTrayApp.__new__(LocalSTTTrayApp)
        app.state_lock=threading.RLock(); app.session_id=1
        app.pressed=set(); app.config=AppConfig(live_dictation=True)
        app.hotkey_down_at=time.perf_counter()-2
        app.recording_mode='dictation'; app.stop_in_progress=False
        app.service=SimpleNamespace(logger=logging.getLogger('test'))
        app._refresh_bindings()
        return app

    def test_typing_during_recording_does_not_stop_it(self):
        from localstt import tray_app as tray
        app=self.app()
        with patch.object(tray.threading,'Thread') as thread:
            for key in [tray.keyboard.KeyCode.from_char('a'),tray.keyboard.KeyCode.from_char('я'),
                        tray.keyboard.Key.space,tray.keyboard.Key.shift]:
                app._on_release(key)
            thread.assert_not_called()
        self.assertFalse(app.stop_in_progress)

    def test_actual_push_to_talk_release_still_stops_recording(self):
        from localstt import tray_app as tray
        app=self.app(); app.pressed={'ctrl','win'}
        with patch.object(tray.threading,'Thread') as thread:
            app._on_release(tray.keyboard.Key.ctrl)
            thread.assert_called_once()
            self.assertTrue(app.stop_in_progress)

    def test_custom_push_to_talk_release(self):
        from localstt import tray_app as tray
        app=self.app(); app.config.hotkey_dictation='f8'; app._refresh_bindings()
        app.pressed={'f8'}
        with patch.object(tray.threading,'Thread') as thread:
            app._on_release(tray.keyboard.Key.f8)
            thread.assert_called_once()
        self.assertTrue(app.stop_in_progress)

    def test_owned_typing_events_are_filtered_but_external_events_are_not(self):
        from localstt import tray_app as tray
        filt=tray.LocalSTTTrayApp._keyboard_event_filter
        self.assertFalse(filt(0,SimpleNamespace(flags=0x10,dwExtraInfo=tray.text_input.INPUT_MARKER)))
        self.assertTrue(filt(0,SimpleNamespace(flags=0,dwExtraInfo=0)))
        self.assertTrue(filt(0,SimpleNamespace(flags=0x10,dwExtraInfo=999)))

    def test_live_completion_respects_clipboard_preference(self):
        for keep, delivered, should_copy in [(False, 5, False), (True, 5, True), (False, 2, True)]:
            with self.subTest(keep=keep, delivered=delivered):
                app=self.app(); app.config.copy_to_clipboard=keep
                for name in ['_stop_recorder_quietly', '_set_state', '_wait_for_hotkeys_released',
                             '_save_last_transcript', '_set_clipboard_text', '_notify', '_finish_recording']:
                    setattr(app,name,Mock())
                live=SimpleNamespace(stopped=threading.Event(),done=Mock(),flush_delivery=Mock(),
                                     text='hello',delivered=delivered,result=lambda:None,error=None)
                app._stop_live_dictation(live)
                self.assertEqual(app._set_clipboard_text.called,should_copy)
                app._finish_recording.assert_called_once()

    def test_live_toggle_is_saved_immediately(self):
        from localstt import settings_window as sw
        from localstt.config import AppConfig
        cfg=AppConfig()
        with patch.object(sw,'_microphone_options',return_value=(['default'],{})), \
             patch.object(sw,'save_config') as save:
            sections=sw.build_sections(cfg,None,logging.getLogger('test'))
            field=next(f for s in sections for f in s.fields if f.key=='live_dictation')
            self.assertIsNotNone(field.setter)
            self.assertTrue(field.setter(True))
            self.assertTrue(cfg.live_dictation)
            save.assert_called_once_with(cfg)
            self.assertTrue(field.getter())

if __name__=='__main__': unittest.main()
