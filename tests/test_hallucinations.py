import unittest

from localstt.dictionary import is_hallucination


class HallucinationTests(unittest.TestCase):
    def test_subtitle_credits_are_dropped(self):
        for credit in (
            " Субтитры создал DimaTorzok",
            "Субтитры сделал DimaTorzok.",
            "СУБТИТРЫ ПОДГОТОВИЛ Алексей",
            "Редактор субтитров А.Синецкая Корректор А.Егорова",
            "Продолжение следует...",
            " Спасибо за просмотр!",
            "Thanks for watching!",
            "Subtitles by the Amara.org community",
        ):
            self.assertTrue(is_hallucination(credit), credit)

    def test_dictated_text_is_kept(self):
        for speech in (
            "Субтитры не работают в плеере",
            "Продолжение следует завтра утром",
            "Спасибо за просмотр и за отзыв о ролике",
            "Субтитры",
            "",
            "  ",
            "Thanks for the review",
        ):
            self.assertFalse(is_hallucination(speech), speech)


if __name__ == "__main__":
    unittest.main()
