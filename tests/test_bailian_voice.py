"""Offline tests: paid-request boundaries and credential handling, no real API calls."""
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
import wave

import httpx

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bailian_voice import VoiceClient, VoiceError, audio_data


class VoiceTests(unittest.TestCase):
    def client(self, handler):
        client = VoiceClient("sk-TEST-SECRET", "cn-beijing", transport=httpx.MockTransport(handler))
        self.addCleanup(client.close)
        return client

    def test_auth_error_does_not_echo_secret_or_raw_response(self):
        def handler(request):
            return httpx.Response(401, json={"code": "InvalidApiKey", "message": "sk-TEST-SECRET"})
        with self.assertRaises(VoiceError) as result:
            self.client(handler).enroll("list_voice")
        self.assertNotIn("sk-TEST-SECRET", str(result.exception))
        self.assertIn("401", str(result.exception))

    def test_api_redirect_does_not_forward_key(self):
        requests = []
        def handler(request):
            requests.append(request)
            return httpx.Response(307, headers={"Location": "https://unrelated.example/api"})
        with self.assertRaises(VoiceError):
            self.client(handler).enroll("list_voice")
        self.assertEqual(len(requests), 1)
        self.assertEqual(requests[0].url.host, "dashscope.aliyuncs.com")

    def test_create_timeout_does_not_retry(self):
        requests = []
        def handler(request):
            requests.append(request)
            raise httpx.ReadTimeout("timeout", request=request)
        with self.assertRaises(VoiceError):
            self.client(handler).enroll("create_voice", target_model="cosyvoice-v3.5-flash")
        self.assertEqual(len(requests), 1)

    def test_model_mismatch_stops_before_synthesis(self):
        requests = []
        def handler(request):
            requests.append(request)
            return httpx.Response(200, json={"output": {"status": "OK", "target_model": "cosyvoice-v2"}})
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(VoiceError):
                self.client(handler).speak("voice", "cosyvoice-v3.5-flash", "hello", Path(folder) / "x.wav")
        self.assertEqual(len(requests), 1)
        self.assertEqual(json.loads(requests[0].content)["input"]["action"], "query_voice")

    def test_synthesis_download_and_redirect_do_not_carry_api_key(self):
        requests = []
        buf = io.BytesIO()
        with wave.open(buf, "wb") as audio:
            audio.setparams((1, 2, 24000, 0, "NONE", "not compressed"))
            audio.writeframes(b"\0\0" * 240)
        wav = buf.getvalue()
        def handler(request):
            requests.append(request)
            if request.url.path.endswith("customization"):
                return httpx.Response(200, json={"output": {"status": "OK", "target_model": "cosyvoice-v3.5-flash"}})
            if request.url.path.endswith("SpeechSynthesizer"):
                return httpx.Response(200, json={"output": {"audio": {"url": "https://audio.example/first.wav"}}})
            if request.url.path.endswith("first.wav"):
                return httpx.Response(302, headers={"Location": "https://storage.example/final.wav"})
            return httpx.Response(200, content=wav)
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "preview.wav"
            self.client(handler).speak("voice", "cosyvoice-v3.5-flash", "hello", output)
            self.assertEqual(output.read_bytes(), wav)
        self.assertEqual(len(requests), 4)
        self.assertTrue(all("Authorization" not in r.headers for r in requests[2:]))

    def test_existing_preview_stops_before_cloud_request(self):
        requests = []
        def handler(request):
            requests.append(request)
            raise AssertionError("No request expected")
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "existing.wav"
            output.write_bytes(b"keep")
            with self.assertRaises(VoiceError):
                self.client(handler).speak("voice", "cosyvoice-v3.5-flash", "hello", output)
            self.assertEqual(output.read_bytes(), b"keep")
        self.assertEqual(requests, [])

    def test_pcm_sample_encoded_locally_without_upload(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "sample.wav"
            with wave.open(str(path), "wb") as audio:
                audio.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
                audio.writeframes(b"\0\0" * 16000 * 10)
            self.assertTrue(audio_data(path).startswith("data:audio/wav;base64,"))


if __name__ == "__main__":
    unittest.main()
