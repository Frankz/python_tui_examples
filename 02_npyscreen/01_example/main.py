import npyscreen

class MyForm(npyscreen.Form):
    def create(self):
        self.myName = self.add(npyscreen.TitleText, name='Nombre:')
        self.myPassword = self.add(npyscreen.TitlePassword, name='Contraseña:')
        self.myButton = self.add(npyscreen.ButtonPress, name='Enviar')
        self.myButton.whenPressed = self.button_pressed

    def button_pressed(self):
        # Acciones a realizar cuando se presiona el botón
        self.parentApp.setNextForm(None)  # Cierra la aplicación
        npyscreen.notify_confirm(f"Nombre: {self.myName.value}\nContraseña: {self.myPassword.value}", title="Información Ingresada")

class MyApp(npyscreen.NPSAppManaged):
    def onStart(self):
        self.addForm('MAIN', MyForm, name='Mi Aplicación TUI')

if __name__ == '__main__':
    app = MyApp()
    app.run()
