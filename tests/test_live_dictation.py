import logging
import threading
import unittest
from types import SimpleNamespace
from localstt.live_dictation import LiveDictation


def result(*items, text=None):
    words = [dict(word=w, start=start, end=end) for w, start, end in items]
    return SimpleNamespace(text=text if text is not None else ''.join(w['word'] for w in words),
                           segments=[{'words': words}], processing_time=0.1)


def service():
    return SimpleNamespace(dictionary=SimpleNamespace(apply=lambda text:text),
        config=SimpleNamespace(command_silence_rms=.008), logger=logging.getLogger('test'))


class LiveTests(unittest.TestCase):
    def make(self, deliver=lambda text:len(text)):
        return LiveDictation(None,service(),deliver)

    def test_confirmed_prefix_and_final_tail_are_not_duplicated(self):
        live=self.make()
        live.accept(result((' Привет',0,.5),(' мир',.5,1)),1.5,flush=False)
        self.assertEqual(live.text,'')
        live.accept(result((' Привет',0,.5),(' мир',.5,1),(' сегодня',1,1.5)),2,flush=False)
        self.assertEqual(live.text,'Привет мир')
        self.assertEqual(live.offset,1)
        live.accept(result((' сегодня.',0,.5)),.8,flush=True)
        self.assertEqual(live.text,'Привет мир сегодня.')
        self.assertAlmostEqual(live.offset,1.8)

    def test_unstable_word_waits(self):
        live=self.make()
        live.accept(result((' wrong',0,.5),(' tail',.5,1)),1,flush=False)
        live.accept(result((' right',0,.5),(' tail',.5,1)),1,flush=False)
        self.assertEqual(live.text,'')
        live.accept(result((' right',0,.5),(' tail',.5,1)),1,flush=False)
        self.assertEqual(live.text,'right')

    def test_pause_flushes_first_result_and_silence_advances(self):
        live=self.make()
        live.accept(result((' Hello.',0,.5)),1.5,flush=True)
        live.accept(result(),1,flush=True)
        self.assertEqual(live.text,'Hello.')
        self.assertEqual(live.offset,2.5)

    def test_long_silence_does_not_stall_finalization(self):
        live=self.make()
        live.accept(result(),25,flush=False)
        self.assertEqual(live.offset,25)

    def test_partial_delivery_only_retries_unsent_suffix(self):
        calls=[]
        def send(text):
            calls.append(text)
            return min(2,len(text))
        live=self.make(send); live.text='hello'
        for _ in range(4): live.flush_delivery()
        self.assertEqual(calls,['hello','llo','o'])
        self.assertEqual(live.delivered,5)

    def test_cancel_prevents_delivery(self):
        live=self.make(lambda text: self.fail('sent after cancel'))
        live.text='hello'; live.cancel(); live.flush_delivery()
        self.assertEqual(live.delivered,0)

    def test_cancel_during_inference_never_accepts_or_delivers(self):
        entered=threading.Event(); release=threading.Event()
        live=self.make(lambda text:self.fail('sent after cancel'))
        live.recorder=SimpleNamespace(duration_seconds=lambda:2,
            tail_silence_seconds=lambda **kw:1,snapshot_to_wav=lambda *a,**kw:2)
        def recognize(*a,**kw):
            entered.set(); release.wait(3)
            return result((' hello',0,1))
        live.service.transcribe=recognize
        live.start(); self.assertTrue(entered.wait(3)); live.cancel(); release.set()
        self.assertTrue(live.done.wait(3)); self.assertEqual(live.text,'')

    def test_audio_arriving_during_inference_is_flushed_after_stop(self):
        live=self.make(); durations=iter([2,3,3])
        live.recorder=SimpleNamespace(duration_seconds=lambda:next(durations),
            tail_silence_seconds=lambda **kw:1)
        snapshots=[]
        def snapshot(path,**kw):
            snapshots.append(kw['start_seconds'])
            return 2 if len(snapshots)==1 else 1
        live.recorder.snapshot_to_wav=snapshot
        responses=iter([result((' hello',0,1)),result((' world',0,.5))])
        def recognize(*a,**kw):
            live.stopped.set()
            return next(responses)
        live.service.transcribe=recognize
        live.run()
        self.assertIsNone(live.error)
        self.assertEqual(snapshots,[0,2])
        self.assertEqual(live.text,'hello world')
        self.assertEqual(live.delivered,len(live.text))

if __name__=='__main__': unittest.main()
