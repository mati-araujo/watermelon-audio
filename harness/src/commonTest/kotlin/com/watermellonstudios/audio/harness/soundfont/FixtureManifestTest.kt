package com.watermellonstudios.audio.harness.soundfont

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertNull

/** MINI-038, D10 — el manifiesto es lo que decide si un fixture está empaquetado. */
class FixtureManifestTest {

    @Test
    fun eachLineIsNameSizeAndSha() {
        val m = FixtureManifest.parse("wma-fixture.sf2 88744 45b2aa30\nwma-fixture.sf3 8026 07056bf3\n")
        assertEquals(FixtureManifest.Entry(88744, "45b2aa30"), m["wma-fixture.sf2"])
        assertEquals(FixtureManifest.Entry(8026, "07056bf3"), m["wma-fixture.sf3"])
    }

    /** Bug que atrapa (D10): un build sin encoder no lista el .sf3, y eso es "no empaquetado". */
    @Test
    fun aFixtureMissingFromTheManifestIsNotPackaged() {
        assertNull(FixtureManifest.parse("wma-fixture.sf2 88744 45b2aa30\n")["wma-fixture.sf3"])
    }

    /** Bug que atrapa: un manifiesto roto leído como "vacío" haría pasar por no empaquetado lo que sí está. */
    @Test
    fun aMalformedManifestIsAnErrorNotAnEmptyOne() {
        assertFailsWith<IllegalArgumentException> { FixtureManifest.parse("wma-fixture.sf2 88744\n") }
        assertFailsWith<IllegalArgumentException> { FixtureManifest.parse("wma-fixture.sf2 muchos 45b2\n") }
    }
}
