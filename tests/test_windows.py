import logging
import os
import unittest
from types import SimpleNamespace
from unittest.mock import patch, Mock


@unittest.skipUnless(os.name == 'nt', 'Windows UI and audio dependencies')
class WindowsTests(unittest.TestCase):
    def test_audio_snapshots_cover_only_uncommitted_frames(self):
        import numpy as np
        from localstt.audio import AudioRecorder
        recorder=AudioRecorder(sample_rate=10)
        recorder._chunks=[np.arange(0,10).reshape(-1,1), np.arange(10,20).reshape(-1,1)]
        recorder._frames=20
        self.assertEqual(recorder._buffered_audio(start_seconds=.7,max_seconds=.8).ravel().tolist(),list(range(7,15)))
        self.assertEqual(recorder._buffered_audio(start_seconds=1.5).ravel().tolist(),list(range(15,20)))
        self.assertIsNone(recorder._buffered_audio(start_seconds=2))
        self.assertEqual(recorder.duration_seconds(),2)

    def test_settings_pages_remain_visible_after_scrolling_and_navigation(self):
        import tkinter as tk
        from localstt import winui, settings_window as sw
        from localstt.config import AppConfig
        winui.enable_dpi_awareness()
        root=tk.Tk(); root.withdraw(); winui.Scale().apply_to(root)
        errors=[]; root.report_callback_exception=lambda *e:errors.append(e)
        ui=SimpleNamespace(root=root,submit=lambda f:root.after(0,f))
        geometry=tk.Toplevel.geometry
        def offscreen(window,value=None):
            return geometry(window, (value.split('+')[0]+'+-10000+-10000') if value else None)
        try:
            with patch.object(winui.UiThread,'instance',return_value=ui), \
                 patch.object(sw.preflight,'_query_gpu',return_value=None), \
                 patch.object(sw,'_run_off_ui'), patch.object(sw,'command_statuses',return_value=[]), \
                 patch.object(sw,'save_config') as save_config, \
                 patch.object(sw.SettingsWindow,'_load_history',return_value=[{'text':'sample','ts':'2026-10-29'}]*60), \
                 patch.object(tk.Toplevel,'geometry',offscreen), \
                 patch.object(tk.Toplevel,'focus_force'), patch.object(tk.Toplevel,'lift'):
                w=sw.SettingsWindow(AppConfig(),logging.getLogger('test'))
                for _ in range(12):
                    for section in w.sections:
                        w._show_section(section.key); root.update()
                        self.assertEqual(w.canvas.canvasy(0),0,section.key)
                        self.assertTrue(w.content.winfo_children(),section.key)
                        self.assertEqual(w.header['text'],section.title)
                        self.assertTrue(w.save_button.winfo_ismapped(),section.key)
                        self.assertLessEqual(w.save_button.winfo_rooty()+w.save_button.winfo_height(),
                                             w.window.winfo_rooty()+w.window.winfo_height())
                        w.canvas.yview_moveto(1); root.update()
                self.assertEqual(errors,[])
                w._show_section('delivery'); root.update()
                self.assertIn('live_dictation',w.controls)
                w.controls['live_dictation']._toggle()
                save_config.assert_called_once_with(w.config)
                self.assertTrue(w.config.live_dictation)
                self.assertNotIn('live_dictation',w.pending)
                w._show_section('audio'); w._show_section('delivery'); root.update()
                self.assertTrue(w.controls['live_dictation'].value)
                w.close()
        finally:
            root.destroy()

    def test_gpu_probe_and_restart_do_not_open_console(self):
        import subprocess
        from localstt import preflight
        from localstt.service import STTService
        from localstt.tray_app import LocalSTTTrayApp
        from pathlib import Path
        with patch.object(preflight,'_nvidia_smi',return_value=Path('nvidia-smi.exe')), \
             patch.object(subprocess,'check_output',return_value='GPU, 580.0, 4096, 512, 8.6') as run:
            self.assertIsNotNone(preflight._query_gpu())
            self.assertEqual(run.call_args.kwargs['creationflags'],subprocess.CREATE_NO_WINDOW)
        with patch.object(Path,'exists',return_value=True), \
             patch.object(subprocess,'check_output',return_value='GPU') as run:
            svc=STTService.__new__(STTService)
            self.assertEqual(svc._detect_gpu_name(),'GPU')
            self.assertEqual(run.call_args.kwargs['creationflags'],subprocess.CREATE_NO_WINDOW)
        with patch.object(subprocess,'Popen') as run:
            app=LocalSTTTrayApp.__new__(LocalSTTTrayApp); app._exit=Mock()
            app._restart()
            self.assertEqual(run.call_args.kwargs['creationflags'],subprocess.CREATE_NO_WINDOW)

    def test_live_delivery_pauses_for_modifiers_and_stops_on_focus_change(self):
        import threading
        from localstt import tray_app as tray
        from localstt.config import AppConfig
        app=tray.LocalSTTTrayApp.__new__(tray.LocalSTTTrayApp)
        app.state_lock=threading.RLock(); app.session_id=1
        app.live_session=SimpleNamespace(cancelled=threading.Event())
        app.live_delivery_blocked=False; app.target_hwnd=123
        app.pressed={'ctrl'}; app.config=AppConfig()
        app.service=SimpleNamespace(logger=logging.getLogger('test'))
        with patch.object(tray,'get_foreground_window',return_value=123), \
             patch.object(tray.text_input,'type_unicode_text',return_value=True) as send:
            self.assertEqual(app._deliver_live_text(1,'hello'),0); send.assert_not_called()
            app.pressed.clear()
            self.assertEqual(app._deliver_live_text(1,'hello'),5)
            self.assertEqual(app._deliver_live_text(2,'stale'),0)
        with patch.object(tray,'get_foreground_window',return_value=456):
            self.assertEqual(app._deliver_live_text(1,'other window'),0)
        self.assertTrue(app.live_delivery_blocked)

if __name__=='__main__': unittest.main()
