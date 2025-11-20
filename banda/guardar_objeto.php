<?php
$conn = new mysqli("localhost", "root", "", "banda_inteligente");

if ($conn->connect_error) {
    die("Error de conexión: " . $conn->connect_error);
}

$tipo = $_POST['tipo'] ?? '';
$color = $_POST['color'] ?? '';
$estado = $_POST['estado'] ?? 'normal';

$stmt = $conn->prepare("INSERT INTO objetos_detectados (tipo, color, estado) VALUES (?, ?, ?)");
$stmt->bind_param("sss", $tipo, $color, $estado);

if ($stmt->execute()) {
    echo "OK";
} else {
    echo "ERROR";
}

$stmt->close();
$conn->close();
?>
