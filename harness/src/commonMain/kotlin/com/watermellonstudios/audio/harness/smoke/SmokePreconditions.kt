package com.watermellonstudios.audio.harness.smoke

/**
 * REQ-053 S1 — las precondiciones que verifica la APP (D4: cada una donde se ve).
 *
 * La ficha de setup (`scripts/smoke-setup.json`) es la única fuente de qué precondiciones tiene
 * cada plan, de qué pasos bloquea cada una y de su remedio. Acá viven sólo los ids que la app
 * EMITE, que tienen que coincidir con los que la ficha declara con `verificador: app`: lo vigila
 * `smoke-device.sh --self-test`. Si la app no emite la línea, el juez la da `no-verificable`, y
 * eso es BLOQUEADO — nunca cumplida.
 */
object SmokePreconditions {
    /** Panel `captura`: el stream de entrada abrió. */
    const val MIC_OPENS: String = "mic-abre"

    /** Panel `usb`: el permiso USB del harness, con su ventana humana (D13). */
    const val USB_PERMISSION: String = "permiso-usb"
}

/** Lo que la app afirma de una precondición: cumplida o no, y lo que vio. */
class PreconditionReading(val met: Boolean, val evidence: String)

/**
 * `mic-abre`: el stream de entrada abrió. Hace falta las dos cosas: que `start()` haya aceptado Y
 * que el stream haya quedado corriendo — aceptar sólo dice que el pedido se encoló.
 */
fun micOpens(accepted: Boolean, running: Boolean, waitedMs: Long): PreconditionReading =
    PreconditionReading(accepted && running, "aceptado:$accepted,corriendo:$running,espera-ms:$waitedMs")

/** Lo que devolvió `connectDevice` dentro de la ventana humana del diálogo. */
enum class DialogOutcome {
    /** No devolvió: la ventana venció. */
    NO_ANSWER,

    /** Devolvió `PERMISSION_DENIED`: la única forma de una negación. */
    DENIED,

    /** Devolvió otra cosa (éxito u otro fallo). */
    OTHER,
}

/**
 * `permiso-usb` al cerrar la ventana del diálogo (D13). Devuelve `null` si no hay nada que
 * afirmar, y entonces NO se emite línea y el juez lo da HUMANO: la ventana venció sin respuesta, o
 * `connectDevice` falló sin ser una negación y UsbManager sigue sin permiso (nadie vio negar).
 *
 * Si el diálogo devolvió algo y UsbManager dice que no hay permiso, NO se decide en ese instante:
 * el diálogo puede seguir abierto, porque un broadcast ajeno con `permission=false` aborta la
 * espera de la librería sin cerrarlo (MINI-040; `smoke-device.sh` manda justamente ese falso).
 * Se espera el resto de la ventana: si el humano acepta, el permiso está (cumplida) y
 * `permiso-falso` sale concluyente (ver [forgedPermissionCheck]). Si no, sólo un
 * `PERMISSION_DENIED` es una negación (incumplida ⇒ BLOQUEADO).
 *
 * @param remainingMs lo que queda de la ventana humana, medido desde que se pidió el diálogo.
 */
suspend fun usbPermissionAfterDialog(
    outcome: DialogOutcome,
    hasPermission: () -> Boolean,
    remainingMs: () -> Long,
    pause: suspend (Long) -> Unit,
): PreconditionReading? {
    if (hasPermission()) {
        val evidence = if (outcome == DialogOutcome.NO_ANSWER) "concedido-sin-respuesta-de-connect" else "dialogo:concedido"
        return PreconditionReading(true, evidence)
    }
    if (outcome == DialogOutcome.NO_ANSWER) return null
    while (true) {
        val left = remainingMs()
        if (left <= 0) break
        pause(minOf(left, PERMISSION_POLL_MS))
        if (hasPermission()) return PreconditionReading(true, "concedido-despues-del-resultado")
    }
    if (outcome != DialogOutcome.DENIED) return null
    return PreconditionReading(false, "resultado=PERMISSION_DENIED,sin-permiso-al-cerrar-la-ventana")
}

const val PERMISSION_POLL_MS = 500L

/** El juicio de `permiso-falso` (REQ-050 S1). [conclusive]: ninguna precondición lo explica. */
class ForgedCheck(val ok: Boolean, val conclusive: Boolean, val reason: String?)

/**
 * `permiso-falso`: que los broadcasts falsos que manda el script no cambiaron nada.
 *
 * Es CONCLUYENTE —el juez no lo bloquea aunque el permiso no se cumpla (REQ-053 S1)— cuando la
 * regresión está probada sin importar lo que haya hecho el humano: un grant que UsbManager
 * desmiente, o un `PERMISSION_DENIED` con el permiso concedido al cerrar la ventana. Una negación
 * sin permiso es ambigua (el humano negó, o un broadcast ajeno abortó y nadie aceptó): falla, pero
 * no es concluyente, y con el permiso negado el juez la da BLOQUEADO (D13).
 */
fun forgedPermissionCheck(forgedGrants: Int, denied: Boolean, permissionAtWindowClose: Boolean): ForgedCheck = when {
    forgedGrants > 0 -> ForgedCheck(false, true, "granted-con-usbmanager-diciendo-que-no")
    denied && permissionAtWindowClose -> ForgedCheck(false, true, "negado-con-el-permiso-concedido:broadcast-ajeno")
    denied -> ForgedCheck(false, false, "negado:humano-o-broadcast-ajeno")
    else -> ForgedCheck(true, false, null)
}
